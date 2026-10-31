#pragma once
#include "sandbox/linux.hpp"
#include <map>
namespace sandbox {
struct EventWatch {
    int fd;
    std::uint32_t tag;
};
// No stored callbacks or borrowed object pointers. Registrations are reconciled on
// each wait; closed/reused descriptor numbers are re-added after MOD returns ENOENT.
class EventLoop {
    UniqueFd epoll_;
    std::map<int, std::uint32_t> registered_;

  public:
    EventLoop();
    std::vector<std::uint32_t> wait(const std::vector<EventWatch> &, int timeout_ms = 20);
};
} // namespace sandbox
