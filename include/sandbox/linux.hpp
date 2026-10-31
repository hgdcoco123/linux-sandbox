#pragma once
#include "sandbox/config.hpp"
#include <chrono>
#include <signal.h>
#include <string_view>
#include <sys/epoll.h>
#include <utility>
namespace sandbox {
class UniqueFd {
  public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() {
        reset();
    }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd_(o.release()) {}
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    int get() const {
        return fd_;
    }
    explicit operator bool() const {
        return fd_ >= 0;
    }
    int release() {
        return std::exchange(fd_, -1);
    }
    void reset(int fd = -1) noexcept;

  private:
    int fd_;
};
struct Pipe {
    UniqueFd read, write;
    static Pipe create();
};
struct SocketPair {
    UniqueFd first, second;
    static SocketPair create();
};
[[noreturn]] void system_error(std::string_view operation);
int checked(int result, std::string_view operation);
UniqueFd open_path(const fs::path &, int flags, int mode = 0600);
UniqueFd open_dir(const fs::path &);
void write_all(int fd, std::string_view);
std::string read_file(const fs::path &, std::size_t max = 1024 * 1024);
void write_control_file(const fs::path &, std::string_view);
void nonblocking(int fd);
void close_from(unsigned first);
void ensure_standard_fds();
void send_pidfd_signal(int fd, int signal);
std::string boot_id();
std::string process_start(pid_t pid);
std::uint64_t monotonic_ms();
class SignalSource {
  public:
    SignalSource();
    ~SignalSource();
    int fd() const {
        return fd_.get();
    }
    bool drain_cancel();

  private:
    sigset_t old_mask_{};
    struct sigaction old_actions_[3]{};
    int numbers_[3]{SIGINT, SIGTERM, SIGCHLD};
    UniqueFd fd_;
};
class Poller {
  public:
    Poller();
    void add(int fd, std::uint32_t tag);
    std::vector<epoll_event> wait(int timeout_ms = 20);

  private:
    UniqueFd fd_;
};
class TickSource {
  public:
    TickSource();
    int fd() const {
        return fd_.get();
    }
    void drain();

  private:
    UniqueFd fd_;
};
} // namespace sandbox
