#include "sandbox/protocol.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
namespace sandbox {
void ControlChannel::send(const std::string &kind, const Json &payload, int pass_fd) {
    if (!payload.is_object())
        throw std::runtime_error("control payload must be an object");
    auto text =
        Json{{"version", 1}, {"sequence", tx_}, {"kind", kind}, {"payload", payload}}.dump();
    if (text.size() > 16384)
        throw std::runtime_error("control message exceeds 16KiB");
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
        throw std::runtime_error("short control message");
    ++tx_;
}
std::optional<Message> ControlChannel::receive(bool nb) {
    char data[16385];
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec io{data, sizeof(data)};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);
    ssize_t n;
    do {
        n = ::recvmsg(fd_.get(), &msg, MSG_CMSG_CLOEXEC | (nb ? MSG_DONTWAIT : 0));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return std::nullopt;
    if (n < 0)
        system_error("recvmsg");
    if (n == 0)
        throw ChannelClosed{};
    std::vector<UniqueFd> received;
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            auto *f = reinterpret_cast<int *>(CMSG_DATA(c));
            for (std::size_t i = 0; i < count; ++i)
                received.emplace_back(f[i]);
        }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n > 16384 || received.size() > 1)
        throw std::runtime_error("invalid bounded control message");
    auto j = Json::parse(data, data + n);
    require_keys(j, {"version", "sequence", "kind", "payload"}, "control message");
    if (j.at("version") != 1 || j.at("sequence").get<std::uint64_t>() != rx_)
        throw std::runtime_error("control version/sequence mismatch");
    if (!j.at("kind").is_string() || !j.at("payload").is_object())
        throw std::runtime_error("invalid control types");
    // Only descriptor-bearing message kinds are permitted to carry an FD.
    // Keep every received FD owned by UniqueFd until validation succeeds.
    const auto kind = j.at("kind").get<std::string>();
    const bool needs_fd = kind == "INIT_CREATED" || kind == "STDIN" || kind == "STDOUT" ||
                          kind == "STDERR";
    if (needs_fd != (received.size() == 1))
        throw std::runtime_error("control message descriptor does not match kind");
    ++rx_;
    Message result{std::move(j), UniqueFd{}};
    if (!received.empty())
        result.fd = std::move(received[0]);
    return result;
}
} // namespace sandbox
