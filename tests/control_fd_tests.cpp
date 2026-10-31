#include "sandbox/protocol.hpp"
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
using namespace sandbox;

namespace {
void check(bool ok) {
    if (!ok)
        throw std::runtime_error("control FD contract failed");
}

struct Pair {
    ControlChannel sender;
    ControlChannel receiver;
    UniqueFd fd;
    Pair()
        : sender{UniqueFd([&] {
              int sockets[2];
              check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
              receiver_fd = sockets[1];
              return sockets[0];
          }())},
          receiver{UniqueFd(receiver_fd)},
          fd(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
        check(static_cast<bool>(fd));
    }
    int receiver_fd = -1;
};

// A message that declares a descriptor must deliver exactly one, and the
// receiver must own it (close-on-exec preserved).
void delivers(const ControlMessage &message) {
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    ControlChannel sender{UniqueFd(sockets[0])};
    ControlChannel receiver{UniqueFd(sockets[1])};
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    check(static_cast<bool>(fd));
    sender.send(message, fd.get());
    auto envelope = receiver.receive();
    check(envelope.has_value());
    check(static_cast<bool>(envelope->fd));
    check((::fcntl(envelope->fd.get(), F_GETFD) & FD_CLOEXEC) != 0);
}

void delivers_body_only(const ControlMessage &message) {
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    ControlChannel sender{UniqueFd(sockets[0])};
    ControlChannel receiver{UniqueFd(sockets[1])};
    sender.send(message);
    auto envelope = receiver.receive();
    check(envelope.has_value());
    check(!envelope->fd);
    check(kind_of(envelope->message) == kind_of(message));
}

// Sending a descriptor for a body-only message, or omitting a required one, is
// rejected by the sender instead of producing an ambiguous frame.
void rejects(const ControlMessage &message, int pass_fd) {
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    ControlChannel sender{UniqueFd(sockets[0])};
    ControlChannel receiver{UniqueFd(sockets[1])};
    bool rejected = false;
    try {
        sender.send(message, pass_fd);
    } catch (const ControlError &) {
        rejected = true;
    }
    check(rejected);
}
} // namespace

int main() {
    delivers(StdinHandle{*CommandIndex::from_number(1)});
    delivers(StdoutHandle{*CommandIndex::from_number(1)});
    delivers(StderrHandle{*CommandIndex::from_number(1)});
    delivers(InitCreated{4242});

    delivers_body_only(EnvReady{});
    delivers_body_only(Prepare{});
    delivers_body_only(MainExit{0, std::nullopt});

    rejects(EnvReady{}, ::open("/dev/null", O_RDONLY));
    rejects(StdinHandle{*CommandIndex::from_number(1)}, -1);
    rejects(InitCreated{1}, -1);

    std::cout << "control descriptor contract passed\n";
}
