#include "sandbox/protocol.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <vector>
namespace sandbox {
void ControlChannel::send(const ControlMessage &message, int pass_fd) {
    const auto kind = kind_of(message);
    const auto contract = fd_contract(kind);
    if (contract.required == 0 && pass_fd >= 0)
        throw ControlError("FD_CONTRACT",
                           std::string(control_kind_name(kind)) + " does not carry a descriptor");
    if (contract.required == 1 && pass_fd < 0)
        throw ControlError("FD_CONTRACT",
                           std::string(control_kind_name(kind)) + " requires a descriptor");
    auto text = encode(message);
    iovec io{const_cast<char *>(text.data()), text.size()};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
    if (pass_fd >= 0) {
        msg.msg_control = ancillary;
        msg.msg_controllen = sizeof(ancillary);
        auto *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    ssize_t n;
    do {
        n = ::sendmsg(fd_.get(), &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        system_error("sendmsg");
    if (static_cast<std::size_t>(n) != text.size())
        throw ControlError("SHORT_WRITE", "short control message");
}
std::optional<ControlEnvelope> ControlChannel::receive(bool nonblock) {
    char data[max_control_packet + 1];
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec io{data, sizeof(data)};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);
    ssize_t n;
    do {
        n = ::recvmsg(fd_.get(), &msg, MSG_CMSG_CLOEXEC | (nonblock ? MSG_DONTWAIT : 0));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return std::nullopt;
    if (n < 0)
        system_error("recvmsg");
    if (n == 0)
        throw ChannelClosed{};
    // Each descriptor is owned by a UniqueFd as soon as it is extracted, so a
    // message rejected for a contract violation cannot leak one.
    std::vector<UniqueFd> received;
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            auto *f = reinterpret_cast<int *>(CMSG_DATA(c));
            for (std::size_t i = 0; i < count; ++i)
                received.emplace_back(f[i]);
        }
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        throw ControlError("BAD_PACKET", "truncated control message or descriptor array");
    auto message = decode(std::string_view(data, static_cast<std::size_t>(n)));
    validate_fd_count(kind_of(message), received.size());
    ControlEnvelope envelope{std::move(message), UniqueFd{}};
    if (!received.empty())
        envelope.fd = std::move(received.front());
    return envelope;
}
} // namespace sandbox
