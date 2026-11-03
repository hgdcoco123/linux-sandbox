#include "sandbox/platform/event_loop.hpp"
#include <array>
#include <cerrno>
#include <set>
namespace sandbox {
EventLoop::EventLoop() : epoll_(checked(::epoll_create1(EPOLL_CLOEXEC), "event epoll")) {}
std::vector<std::uint32_t> EventLoop::wait(const std::vector<EventWatch> &watches, int timeout) {
    std::map<int, std::uint32_t> desired;
    std::set<std::uint32_t> tags;
    for (auto watch : watches)
        if (watch.fd >= 0) {
            if (!desired.emplace(watch.fd, watch.tag).second || !tags.insert(watch.tag).second)
                throw std::logic_error("duplicate event registration");
        }
    for (auto [fd, tag] : registered_)
        if (!desired.contains(fd)) {
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != EBADF &&
                errno != ENOENT)
                system_error("event remove");
        }
    for (auto [fd, tag] : desired) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        event.data.u32 = tag;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &event) < 0) {
            if (errno != ENOENT)
                system_error("event modify");
            checked(::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event), "event add");
        }
    }
    registered_ = std::move(desired);
    std::array<epoll_event, 32> events{};
    int count;
    do {
        count = ::epoll_wait(epoll_.get(), events.data(), events.size(), timeout);
    } while (count < 0 && errno == EINTR);
    checked(count, "event wait");
    std::vector<std::uint32_t> result;
    for (int i = 0; i < count; ++i)
        result.push_back(events[i].data.u32);
    return result;
}
} // namespace sandbox
