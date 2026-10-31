#include "sandbox/launcher.hpp"
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace sandbox {
namespace {
struct SpawnActions {
    posix_spawn_file_actions_t value;
    SpawnActions() {
        int r = posix_spawn_file_actions_init(&value);
        if (r)
            throw std::runtime_error("spawn actions init");
    }
    ~SpawnActions() {
        posix_spawn_file_actions_destroy(&value);
    }
};
void spawn_check(int rc, const char *op) {
    if (rc) {
        errno = rc;
        system_error(op);
    }
}
struct SpawnAttrs {
    posix_spawnattr_t value;
    SpawnAttrs() {
        spawn_check(posix_spawnattr_init(&value), "spawn attrs");
    }
    ~SpawnAttrs() {
        posix_spawnattr_destroy(&value);
    }
};
} // namespace
LauncherClient::LauncherClient(const RunConfig &c, const fs::path &executable, const fs::path &root,
                               int input, int output, int error)
    : launcher_(UniqueFd{}), init_(UniqueFd{}) {
    auto p = SocketPair::create(), i = SocketPair::create();
    launcher_ = ControlChannel(std::move(p.first));
    init_ = ControlChannel(std::move(i.first));
    auto spec = UniqueFd(checked(static_cast<int>(::syscall(SYS_memfd_create, "sandbox-spec",
                                                            MFD_CLOEXEC | MFD_ALLOW_SEALING)),
                                 "memfd_create"));
    auto text = Json{{"config", c.to_json()}, {"root_mountpoint", root.string()}}.dump();
    if (text.size() > 1024 * 1024)
        throw std::runtime_error("bootstrap spec too large");
    write_all(spec.get(), text);
    checked(static_cast<int>(::lseek(spec.get(), 0, SEEK_SET)), "rewind bootstrap");
    checked(
        ::fcntl(spec.get(), F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL),
        "seal bootstrap");
    std::vector<UniqueFd> sources;
    if (c.mode == "isolated") {
        sources.push_back(open_path(c.runtime_root, O_PATH | O_DIRECTORY));
        for (const auto &m : c.mounts)
            sources.push_back(open_path(m.source, O_PATH | O_DIRECTORY));
    }
    // Duplicate all inputs first: spawn dup2 actions must not overwrite later source FDs.
    std::vector<int> original{input, output, error, p.second.get(), spec.get(), i.second.get()};
    for (auto &s : sources)
        original.push_back(s.get());
    std::vector<UniqueFd> backups;
    for (auto fd : original)
        backups.emplace_back(checked(::fcntl(fd, F_DUPFD_CLOEXEC, 100), "backup spawn fd"));
    SpawnActions actions;
    for (std::size_t n = 0; n < backups.size(); ++n)
        spawn_check(
            posix_spawn_file_actions_adddup2(&actions.value, backups[n].get(), static_cast<int>(n)),
            "spawn dup2");
    spawn_check(
        posix_spawn_file_actions_addclosefrom_np(&actions.value, static_cast<int>(original.size())),
        "spawn closefrom");
    SpawnAttrs attrs;
    sigset_t empty, defaults;
    ::sigemptyset(&empty);
    ::sigfillset(&defaults);
    spawn_check(posix_spawnattr_setsigmask(&attrs.value, &empty), "spawn sigmask");
    spawn_check(posix_spawnattr_setsigdefault(&attrs.value, &defaults), "spawn sigdefault");
    spawn_check(
        posix_spawnattr_setflags(&attrs.value, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF),
        "spawn flags");
    auto path = executable.string();
    char *args[]{path.data(), nullptr};
    char envpath[] = "PATH=/usr/bin:/bin";
    char *env[]{envpath, nullptr};
    spawn_check(::posix_spawn(&pid_, path.c_str(), &actions.value, &attrs.value, args, env),
                "spawn launcher");
    launcher_pidfd_.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0)));
    if (!launcher_pidfd_) {
        int saved = errno;
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
        errno = saved;
        system_error("pidfd launcher");
    }
}
LauncherClient::~LauncherClient() {
    if (!reaped_) {
        try {
            abort();
        } catch (...) {
        }
    }
}
void LauncherClient::set_init_identity(pid_t p, UniqueFd fd) {
    if (init_pid_ > 0 || p <= 0 || !fd)
        throw std::runtime_error("invalid init identity");
    init_pid_ = p;
    init_pidfd_ = std::move(fd);
}
bool LauncherClient::poll_reaped() {
    if (reaped_)
        return true;
    int status;
    auto n = ::waitpid(pid_, &status, WNOHANG);
    if (n == pid_) {
        reaped_ = true;
        return true;
    }
    if (n < 0 && errno != EINTR)
        system_error("wait launcher");
    return false;
}
void LauncherClient::abort() {
    if (init_pidfd_)
        send_pidfd_signal(init_pidfd_.get(), SIGKILL);
    // Keep launcher alive long enough to reap init after observing control EOF.
    launcher_.close();
    init_.close();
    auto end = monotonic_ms() + 1500;
    while (!poll_reaped() && monotonic_ms() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!reaped_) {
        send_pidfd_signal(launcher_pidfd_.get(), SIGKILL);
        int status;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR)
                system_error("reap aborted launcher");
        }
        reaped_ = true;
    }
}
} // namespace sandbox
