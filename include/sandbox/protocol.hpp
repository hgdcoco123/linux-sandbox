#pragma once
#include "sandbox/core/control.hpp"
#include "sandbox/linux.hpp"
#include <optional>
namespace sandbox {
struct ChannelClosed : std::runtime_error {
    ChannelClosed() : std::runtime_error("control channel closed") {}
};
// A decoded control message and the descriptor it legitimately carried.
struct ControlEnvelope {
    ControlMessage message;
    UniqueFd fd;
};
class ControlChannel {
  public:
    explicit ControlChannel(UniqueFd fd) : fd_(std::move(fd)) {}
    ControlChannel(ControlChannel &&) = default;
    ControlChannel &operator=(ControlChannel &&) = default;
    int fd() const {
        return fd_.get();
    }
    // The descriptor count is part of each message's contract; passing one for a
    // body-only message is a programming error rather than a runtime case.
    void send(const ControlMessage &, int pass_fd = -1);
    std::optional<ControlEnvelope> receive(bool nonblock = false);
    void close() {
        fd_.reset();
    }

  private:
    UniqueFd fd_;
};
} // namespace sandbox
