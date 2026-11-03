#pragma once
#include "sandbox/model/config.hpp"
#include <chrono>
#include <signal.h>
#include <string_view>
#include <sys/epoll.h>
#include <utility>
#include <vector>
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
// Create `paths` in order as one transaction. A path that already exists as a
// directory is reused, never removed. If a later creation fails, exactly the
// directories this call created are removed again - reverse order, rmdir only, so a
// failure here cannot widen into a recursive delete - and the thrown message keeps the
// original failure first and appends any removal that itself failed.
void create_directories_or_remove(const std::vector<fs::path> &paths);
// Remove every path, attempting all of them even when one fails: an undo must not stop
// at its first failure (the rule PrepareSequence already follows). Empty paths are
// skipped and a missing path counts as removed. Returns every failure as one message,
// or an empty string when everything was removed.
std::string remove_all_collect(const std::vector<fs::path> &paths);
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
