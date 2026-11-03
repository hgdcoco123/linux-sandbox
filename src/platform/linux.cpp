#include "sandbox/platform/linux.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
namespace sandbox {
void create_directories_or_remove(const std::vector<fs::path> &paths) {
    std::vector<fs::path> created;
    std::string reason;
    for (const auto &path : paths) {
        if (::mkdir(path.c_str(), 0700) == 0) {
            created.push_back(path);
            continue;
        }
        const int error = errno;
        // A directory that is already there is reused, not removed: it is not this
        // call's to delete, and a retried preparation must be able to go on.
        struct stat existing {};
        if (error == EEXIST && ::lstat(path.c_str(), &existing) == 0 && S_ISDIR(existing.st_mode))
            continue;
        reason = "create directory " + path.string() + ": " + std::strerror(error);
        break;
    }
    if (reason.empty())
        return;
    // Reclaim only what this call created: reverse order, rmdir only. A non-empty
    // directory survives rather than being deleted recursively - a cleanup that widens
    // into the tree is a worse failure than a leftover.
    std::string leftovers;
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        if (::rmdir(it->c_str()) == 0)
            continue;
        const int error = errno;
        leftovers += "\n  rollback: rmdir " + it->string() + ": " + std::strerror(error);
    }
    // The original reason comes first; a failed removal is a diagnostic, not a
    // replacement for it, and never silently dropped.
    throw std::runtime_error(reason + leftovers);
}
std::string remove_all_collect(const std::vector<fs::path> &paths) {
    std::string failures;
    for (const auto &path : paths) {
        if (path.empty())
            continue;
        std::error_code failure;
        fs::remove_all(path, failure);
        if (!failure)
            continue;
        if (!failures.empty())
            failures += "; ";
        failures += path.string() + ": " + failure.message();
    }
    return failures;
}
void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = fd;
}
[[noreturn]] void system_error(std::string_view op) {
    throw std::system_error(errno, std::generic_category(), std::string(op));
}
int checked(int n, std::string_view op) {
    if (n < 0)
        system_error(op);
    return n;
}
Pipe Pipe::create() {
    int f[2];
    checked(::pipe2(f, O_CLOEXEC), "pipe2");
    return {UniqueFd(f[0]), UniqueFd(f[1])};
}
SocketPair SocketPair::create() {
    int f[2];
    checked(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, f), "socketpair");
    return {UniqueFd(f[0]), UniqueFd(f[1])};
}
UniqueFd open_path(const fs::path &p, int flags, int mode) {
    return UniqueFd(
        checked(::open(p.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, mode), "open " + p.string()));
}
UniqueFd open_dir(const fs::path &p) {
    return open_path(p, O_RDONLY | O_DIRECTORY);
}
void write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        auto n = ::write(fd, s.data(), s.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            system_error("write");
        }
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}
std::string read_file(const fs::path &p, std::size_t max) {
    auto fd = open_path(p, O_RDONLY);
    std::string out;
    char buf[4096];
    for (;;) {
        auto n = ::read(fd.get(), buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            system_error("read " + p.string());
        if (!n)
            break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > max)
            throw std::runtime_error("file exceeds bound: " + p.string());
    }
    return out;
}
void write_control_file(const fs::path &p, std::string_view s) {
    auto fd = open_path(p, O_WRONLY);
    write_all(fd.get(), s);
}
void nonblocking(int fd) {
    auto f = checked(::fcntl(fd, F_GETFL), "F_GETFL");
    checked(::fcntl(fd, F_SETFL, f | O_NONBLOCK), "F_SETFL");
}
void close_from(unsigned first) {
    checked(static_cast<int>(::syscall(SYS_close_range, first, ~0U, 0)), "close_range");
}
void ensure_standard_fds() {
    for (int i = 0; i < 3; ++i) {
        if (::fcntl(i, F_GETFD) >= 0)
            continue;
        if (errno != EBADF)
            system_error("standard fd");
        auto f = checked(::open("/dev/null", O_RDWR), "/dev/null");
        if (f != i) {
            ::close(f);
            throw std::runtime_error("standard fd collision");
        }
    }
}
void send_pidfd_signal(int fd, int sig) {
    if (fd < 0)
        return;
    if (::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0) < 0 && errno != ESRCH)
        system_error("pidfd_send_signal");
}
std::string boot_id() {
    auto s = read_file("/proc/sys/kernel/random/boot_id", 128);
    if (!s.empty() && s.back() == '\n')
        s.pop_back();
    return s;
}
std::string process_start(pid_t pid) {
    auto s = read_file("/proc/" + std::to_string(pid) + "/stat", 4096);
    auto pos = s.rfind(')');
    if (pos == std::string::npos)
        throw std::runtime_error("bad proc stat");
    std::istringstream in(s.substr(pos + 2));
    std::string field;
    for (int i = 3; i <= 22; ++i)
        if (!(in >> field))
            throw std::runtime_error("short proc stat");
    return field;
}
std::uint64_t monotonic_ms() {
    timespec ts{};
    checked(::clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000 +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}
SignalSource::SignalSource() {
    sigset_t mask;
    ::sigemptyset(&mask);
    for (auto n : numbers_)
        ::sigaddset(&mask, n);
    checked(::sigprocmask(SIG_BLOCK, &mask, &old_mask_), "block signals");
    std::size_t done = 0;
    try {
        for (; done < 3; ++done) {
            struct sigaction a{};
            a.sa_handler = SIG_DFL;
            ::sigemptyset(&a.sa_mask);
            checked(::sigaction(numbers_[done], &a, &old_actions_[done]), "signal disposition");
        }
        fd_.reset(checked(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK), "signalfd"));
    } catch (...) {
        for (std::size_t i = 0; i < done; ++i)
            ::sigaction(numbers_[i], &old_actions_[i], nullptr);
        ::sigprocmask(SIG_SETMASK, &old_mask_, nullptr);
        throw;
    }
}
SignalSource::~SignalSource() {
    for (int i = 0; i < 3; ++i)
        ::sigaction(numbers_[i], &old_actions_[i], nullptr);
    ::sigprocmask(SIG_SETMASK, &old_mask_, nullptr);
}
bool SignalSource::drain_cancel() {
    bool cancel = false;
    signalfd_siginfo info{};
    for (;;) {
        auto n = ::read(fd_.get(), &info, sizeof(info));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n != sizeof(info))
            throw std::runtime_error("invalid signal event");
        cancel = cancel || info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM;
    }
    return cancel;
}
Poller::Poller() : fd_(checked(::epoll_create1(EPOLL_CLOEXEC), "epoll_create1")) {}
void Poller::add(int fd, std::uint32_t tag) {
    epoll_event e{};
    e.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    e.data.u32 = tag;
    checked(::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &e), "epoll add");
}
std::vector<epoll_event> Poller::wait(int timeout) {
    std::vector<epoll_event> es(16);
    int n;
    do {
        n = ::epoll_wait(fd_.get(), es.data(), static_cast<int>(es.size()), timeout);
    } while (n < 0 && errno == EINTR);
    checked(n, "epoll_wait");
    es.resize(static_cast<std::size_t>(n));
    return es;
}
TickSource::TickSource()
    : fd_(checked(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK), "timerfd")) {
    itimerspec s{};
    s.it_value.tv_nsec = 20000000;
    s.it_interval = s.it_value;
    checked(::timerfd_settime(fd_.get(), 0, &s, nullptr), "timerfd_settime");
}
void TickSource::drain() {
    std::uint64_t ticks;
    while (::read(fd_.get(), &ticks, sizeof(ticks)) > 0) {
    }
}
} // namespace sandbox
