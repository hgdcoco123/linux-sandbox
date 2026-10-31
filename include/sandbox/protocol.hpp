#pragma once
#include "sandbox/linux.hpp"
#include <optional>
namespace sandbox {
struct ChannelClosed : std::runtime_error {
    ChannelClosed() : std::runtime_error("control channel closed") {}
};
struct Message {
    Json body;
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
    void send(const std::string &kind, const Json &payload = Json::object(), int pass_fd = -1);
    std::optional<Message> receive(bool nonblock = false);
    void close() {
        fd_.reset();
    }

  private:
    UniqueFd fd_;
    std::uint64_t tx_ = 0, rx_ = 0;
};
} // namespace sandbox
