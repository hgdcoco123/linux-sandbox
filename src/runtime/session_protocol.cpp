#include "sandbox/session_protocol.hpp"
#include "sandbox/session_wire.hpp"
#include "sandbox/session_store.hpp"
#include "sandbox/store.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
namespace sandbox {
Json ControlServer::request(const fs::path &root, const std::string &id, const Json &request) {
    auto directory = RunStore::verified_run(root, id);
    // Read-only reconciliation remains available after the control host exits.
    if (request.is_object() && request.value("op", "") == "request-status" &&
        !RunStore::inspect(root, id).at("active").get<bool>()) {
        auto parsed = validate_session_request(request);
        auto key = RequestKey::parse(parsed.at("request_id").get<std::string>());
        if (!key)
            throw SessionError("INVALID_REQUEST", "invalid request_id");
        return session_wire::command_report_to_json(SessionStore::lookup_request(directory, *key));
    }
    auto path = directory / "control.sock";
    UniqueFd fd(
        checked(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), "session client socket"));
    timeval timeout{5, 0};
    checked(::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
            "client receive timeout");
    checked(::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)),
            "client send timeout");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto name = path.string();
    if (name.size() >= sizeof(addr.sun_path))
        throw std::runtime_error("socket path too long");
    std::copy(name.begin(), name.end(), addr.sun_path);
    checked(::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
            "connect session");
    // Batch control shares this CLI entry point but retains its JSON protocol.
    bool binary = !fs::exists(directory / "batch.json");
    auto data = binary ? session_wire::encode_request(request) : request.dump();
    if (data.size() > 16384)
        throw std::runtime_error("request exceeds 16KiB");
    auto sent = ::send(fd.get(), data.data(), data.size(), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(data.size()))
        throw std::runtime_error("request send failed");
    char response[16384];
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 16)]{};
    iovec iov{response, sizeof(response)}; msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = ancillary; message.msg_controllen = sizeof(ancillary);
    auto n = ::recvmsg(fd.get(), &message, MSG_TRUNC | MSG_CMSG_CLOEXEC);
    bool unexpected_control = (message.msg_flags & MSG_CTRUNC) != 0;
    for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        unexpected_control = true;
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
            auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            auto *fds = reinterpret_cast<int *>(CMSG_DATA(c));
            for (std::size_t i = 0; i < count; ++i) ::close(fds[i]);
        }
    }
    if (n <= 0 || n > static_cast<ssize_t>(sizeof(response)) ||
        (message.msg_flags & MSG_TRUNC) || unexpected_control)
        throw std::runtime_error("invalid/missing response");
    if (binary)
        return session_wire::decode_response(std::string_view(response, static_cast<std::size_t>(n)));
    auto answer = Json::parse(response, response + n);
    if (!answer.value("ok", false)) {
        auto error = answer.value("error_detail", Json::object());
        throw SessionError(
            error.value("code", "OPERATION_FAILED"),
            error.value("message", answer.value("error", "session rejected request")),
            error.value("retryable", false));
    }
    return answer.at("data");
}
} // namespace sandbox
