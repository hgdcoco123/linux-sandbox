#pragma once
#include "sandbox/protocol.hpp"
#include <optional>
namespace sandbox {
class LauncherClient {
  public:
    LauncherClient(const RunConfig &, const fs::path &launcher, const fs::path &root_mountpoint,
                   int input, int output, int error);
    ~LauncherClient();
    ControlChannel &launcher_channel() {
        return launcher_;
    }
    ControlChannel &init_channel() {
        return init_;
    }
    void set_init_identity(pid_t host_pid, UniqueFd pidfd);
    pid_t init_pid() const {
        return init_pid_;
    }
    int init_pidfd() const {
        return init_pidfd_.get();
    }
    bool poll_reaped();
    void abort();

  private:
    pid_t pid_ = -1, init_pid_ = -1;
    bool reaped_ = false;
    UniqueFd launcher_pidfd_, init_pidfd_;
    ControlChannel launcher_, init_;
};
} // namespace sandbox
