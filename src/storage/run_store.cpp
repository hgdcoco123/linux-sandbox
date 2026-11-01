#include "sandbox/cgroup.hpp"
#include "sandbox/core/ops_json.hpp"
#include "sandbox/session_store.hpp"
#include "sandbox/session_wire.hpp"
#include "sandbox/store.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace sandbox {
namespace {
void secure_dir(const fs::path &p) {
    struct stat s{};
    checked(::lstat(p.c_str(), &s), "lstat state dir");
    if (!S_ISDIR(s.st_mode) || s.st_uid != ::getuid() || (s.st_mode & 0077))
        throw std::runtime_error("state directory must be user-owned and private (0700): " +
                                 p.string());
}
sockaddr_un address(const fs::path &p) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    auto s = p.string();
    if (s.size() >= sizeof(a.sun_path))
        throw std::runtime_error("UNIX socket path too long; choose shorter --state-root");
    std::memcpy(a.sun_path, s.c_str(), s.size() + 1);
    return a;
}
std::string new_id() {
    unsigned char data[12];
    auto n = ::getrandom(data, sizeof(data), 0);
    if (n != sizeof(data))
        throw std::runtime_error("getrandom failed/short");
    std::string s = "run-";
    const char *hex = "0123456789abcdef";
    for (auto c : data) {
        s += hex[c >> 4];
        s += hex[c & 15];
    }
    return s;
}
} // namespace
RunStore::RunStore(fs::path p, std::string id, UniqueFd lock, UniqueFd dir)
    : directory_(std::move(p)), id_(std::move(id)), lock_(std::move(lock)),
      directory_fd_(std::move(dir)) {}
std::unique_ptr<RunStore> RunStore::create(const fs::path &input, const RunConfig &config) {
    auto root = fs::absolute(input);
    if (!fs::exists(root)) {
        fs::create_directories(root);
        checked(::chmod(root.c_str(), 0700), "chmod state root");
    }
    secure_dir(root);
    root = fs::canonical(root);
    auto id = new_id();
    auto path = root / id;
    checked(::mkdir(path.c_str(), 0700), "create run directory");
    auto dir = open_dir(path);
    auto lock = open_path(path / "lock", O_RDWR | O_CREAT | O_EXCL);
    checked(::flock(lock.get(), LOCK_EX | LOCK_NB), "lock new run");
    auto store = std::unique_ptr<RunStore>(new RunStore(path, id, std::move(lock), std::move(dir)));
    store->owner_ = {{"schema_version", 1},
                     {"run_id", id},
                     {"uid", ::getuid()},
                     {"boot_id", boot_id()},
                     {"supervisor_pid", ::getpid()},
                     {"supervisor_start", process_start(::getpid())},
                     {"mode", config.mode},
                     {"cgroup_root", config.cgroup_root.string()}};
    store->write_json("owner.json", store->owner_);
    store->write_json("config.json", config.to_json());
    store->state(RunState::created);
    return store;
}
void RunStore::write_json(const std::string &name, const Json &value) {
    auto temp = name + ".tmp";
    auto f = UniqueFd(checked(::openat(directory_fd_.get(), temp.c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600),
                              "open state temp"));
    auto text = value.dump(2, ' ', false, Json::error_handler_t::replace);
    write_all(f.get(), text);
    checked(::fsync(f.get()), "fsync state");
    checked(::renameat(directory_fd_.get(), temp.c_str(), directory_fd_.get(), name.c_str()),
            "rename state");
    auto parent = fs::path(name).parent_path();
    if (!parent.empty()) {
        auto child = open_dir(directory_ / parent);
        checked(::fsync(child.get()), "fsync command dir");
    }
    checked(::fsync(directory_fd_.get()), "fsync state dir");
}
void RunStore::state(RunState s, const Json &extra) {
    Json j = {{"schema_version", 1},
              {"run_id", id_},
              {"state", state_name(s)},
              {"updated_monotonic_ms", monotonic_ms()}};
    j.update(extra);
    write_json("state.json", j);
}
void RunStore::owner_update(const Json &extra) {
    owner_.update(extra);
    write_json("owner.json", owner_);
}
void RunStore::report(const RunReport &r) {
    write_json("result.json", r.json());
}
fs::path RunStore::verified_run(const fs::path &root, const std::string &id) {
    if (id.size() != 28 || id.rfind("run-", 0) != 0 ||
        id.find_first_not_of("0123456789abcdef", 4) != std::string::npos)
        throw std::runtime_error("invalid run ID");
    secure_dir(root);
    auto p = fs::canonical(root) / id;
    secure_dir(p);
    return p;
}
Json RunStore::inspect(const fs::path &root, const std::string &id) {
    auto p = verified_run(root, id);
    Json j = {{"state", Json::parse(read_file(p / "state.json"))}};
    if (fs::exists(p / "result.json"))
        j["result"] = Json::parse(read_file(p / "result.json"));
    auto lock = open_path(p / "lock", O_RDWR);
    bool active = ::flock(lock.get(), LOCK_EX | LOCK_NB) < 0;
    if (active && errno != EWOULDBLOCK)
        system_error("inspect lock");
    j["active"] = active;
    return j;
}
Json RunStore::cleanup(const fs::path &root) {
    secure_dir(root);
    Json results = Json::array();
    for (const auto &e : fs::directory_iterator(root)) {
        auto id = e.path().filename().string();
        if (id.rfind("run-", 0) != 0)
            continue;
        try {
            auto p = verified_run(root, id);
            auto lock = open_path(p / "lock", O_RDWR);
            if (::flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
                if (errno == EWOULDBLOCK) {
                    results.push_back({{"run_id", id}, {"status", "active"}});
                    continue;
                }
                system_error("cleanup lock");
            }
            auto owner = Json::parse(read_file(p / "owner.json"));
            if (owner.at("schema_version") != 1 || owner.at("run_id") != id ||
                owner.at("uid").get<unsigned>() != ::getuid())
                throw std::runtime_error("owner mismatch");
            if (owner.at("mode") == "isolated") {
                auto cgroot = fs::path(owner.at("cgroup_root").get<std::string>());
                CgroupV2::recover(cgroot, id);
            } else if (!fs::exists(p / "result.json")) {
                throw std::runtime_error(
                    "process backend stale: cannot prove descendant ownership; no PID killed");
            }
            auto sock = p / "control.sock";
            struct stat st{};
            if (::lstat(sock.c_str(), &st) == 0) {
                if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
                    throw std::runtime_error("unexpected control socket");
                checked(::unlink(sock.c_str()), "unlink stale socket");
            }
            if (owner.value("persistent", false) && !fs::exists(p / "result.json")) {
                auto session = SessionStore::read(p / "session.json");
                RunStore recovered(p, id,
                                   UniqueFd(checked(::fcntl(lock.get(), F_DUPFD_CLOEXEC, 3),
                                                    "duplicate recovery lock")),
                                   open_dir(p));
                SessionStore records(recovered);
                session = records.recover(session);
                session["session_state"] = "closed";
                session["stop_reason"] = "recovered_after_interruption";
                session["errors"] = {"execution outcome unknown after supervisor interruption"};
                records.close(session);
            }
            Json recovery = {{"schema_version", 1},
                             {"run_id", id},
                             {"status", "resources_recovered"},
                             {"boot_id", boot_id()}};
            auto recovery_file = open_path(p / "recovery.json", O_WRONLY | O_CREAT | O_TRUNC);
            write_all(recovery_file.get(), recovery.dump(2));
            results.push_back(recovery);
        } catch (const std::exception &ex) {
            results.push_back({{"run_id", id}, {"status", "unresolved"}, {"error", ex.what()}});
        }
    }
    return results;
}
ControlServer::ControlServer(const fs::path &path)
    : path_(path), fd_(checked(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0),
                               "control socket")) {
    auto a = address(path);
    checked(::bind(fd_.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)), "bind control");
    checked(::chmod(path.c_str(), 0600), "chmod socket");
    checked(::listen(fd_.get(), 8), "listen control");
}
ControlServer::~ControlServer() {
    ::unlink(path_.c_str());
}
bool ControlServer::accept_cancel() {
    bool result = false;
    serve([&](const SessionRequest &request) -> Json {
        bool ok = request.as<Cancel>() != nullptr;
        result = result || ok;
        return {{"status", ok ? "accepted" : "rejected"}};
    });
    return result;
}
void ControlServer::serve(const std::function<Json(const SessionRequest &)> &handler,
                          const std::function<Json(const Json &)> &json_handler) {
    for (int budget = 0; budget < 8; ++budget) {
        UniqueFd peer(::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
        if (!peer) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            system_error("accept");
        }
        ucred cred{};
        socklen_t size = sizeof(cred);
        checked(::getsockopt(peer.get(), SOL_SOCKET, SO_PEERCRED, &cred, &size),
                "peer credentials");
        if (cred.uid != ::getuid() || pending_.size() >= 16) {
            ::send(peer.get(), "rejected", 8, MSG_NOSIGNAL | MSG_DONTWAIT);
            continue;
        }
        pending_.push_back({std::move(peer), monotonic_ms() + 500});
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        char request[16384];
        alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 16)]{};
        iovec iov{request, sizeof(request)};
        msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
        message.msg_control = ancillary; message.msg_controllen = sizeof(ancillary);
        auto n = ::recvmsg(it->fd.get(), &message, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
            monotonic_ms() < it->deadline) {
            ++it;
            continue;
        }
        std::string response;
        bool binary = n >= static_cast<ssize_t>(session_wire::magic.size()) &&
                      std::string_view(request, session_wire::magic.size()) == session_wire::magic;
        bool unexpected_control = (message.msg_flags & MSG_CTRUNC) != 0;
        for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
            unexpected_control = true;
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
                auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                auto *fds = reinterpret_cast<int *>(CMSG_DATA(c));
                for (std::size_t i = 0; i < count; ++i) ::close(fds[i]);
            }
        }
        try {
            if (n <= 0 || n > static_cast<ssize_t>(sizeof(request)) ||
                (message.msg_flags & MSG_TRUNC) || unexpected_control)
                throw std::runtime_error("invalid request size");
            bool legacy = n == 6 && std::string_view(request, 6) == "cancel";
            // One decode, into business types. The JSON front door below shares the
            // same target, so a client cannot reach the controller with a value the
            // protobuf path would have rejected.
            std::optional<SessionRequest> decoded;
            std::string operation;
            if (binary) {
                auto typed = session_wire::decode_session_request(
                    std::string_view(request, static_cast<std::size_t>(n)));
                decoded = std::move(typed.request);
                operation = std::string(session_wire::operation_name(decoded->operation()));
            } else if (!legacy && json_handler) {
                auto answer = json_handler(Json::parse(request, request + n));
                response =
                    Json{{"protocol_version", 2}, {"ok", true}, {"data", answer}}.dump();
                if (response.size() > 16384)
                    response = R"({"ok":false,"error":"response too large"})";
                ::send(it->fd.get(), response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                it = pending_.erase(it);
                continue;
            } else {
                auto parsed = legacy ? Json{{"op", "cancel"}} : Json::parse(request, request + n);
                std::string error;
                decoded = session_request_from_json(parsed, error);
                if (!decoded)
                    throw SessionError("INVALID_REQUEST", error);
                operation = std::string(session_wire::operation_name(decoded->operation()));
            }
            auto answer = handler(*decoded);
            response = binary ? session_wire::encode_response(operation, answer)
                       : legacy ? answer.value("status", "rejected")
                                : Json{{"protocol_version", 2}, {"ok", true}, {"data", answer}}.dump();
        } catch (const SessionError &e) {
            response = binary ? session_wire::encode_error(e) : Json{
                {"protocol_version", 2},
                {"ok", false},
                {"error", e.what()},
                {"error_detail",
                 e.json()}}.dump();
        } catch (const Json::exception &e) {
            SessionError error("INVALID_REQUEST", e.what());
            response = binary ? session_wire::encode_error(error) : Json{{"protocol_version", 2},
                            {"ok", false},
                            {"error", e.what()},
                            {"error_detail", error.json()}}
                           .dump();
        } catch (const std::exception &e) {
            SessionError error("OPERATION_FAILED", e.what());
            response = binary ? session_wire::encode_error(error) : Json{{"protocol_version", 2},
                            {"ok", false},
                            {"error", e.what()},
                            {"error_detail", error.json()}}
                           .dump();
        }
        if (response.size() > 16384)
            response = R"({"ok":false,"error":"response too large"})";
        ::send(it->fd.get(), response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        it = pending_.erase(it);
    }
}
Json ControlServer::cancel(const fs::path &root, const std::string &id) {
    auto p = RunStore::verified_run(root, id);
    if (fs::exists(p / "result.json"))
        return {{"run_id", id}, {"status", "already_finished"}};
    auto fd =
        UniqueFd(checked(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), "cancel socket"));
    timeval timeout{2, 0};
    checked(::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
            "cancel timeout");
    auto a = address(p / "control.sock");
    checked(::connect(fd.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)), "connect cancel");
    checked(static_cast<int>(::send(fd.get(), "cancel", 6, MSG_NOSIGNAL)), "send cancel");
    char b[32];
    auto n = ::recv(fd.get(), b, sizeof(b), 0);
    if (n <= 0)
        throw std::runtime_error("cancel acknowledgement unavailable");
    return {{"run_id", id}, {"status", std::string(b, static_cast<std::size_t>(n))}};
}
} // namespace sandbox
