#include "sandbox/protocol.hpp"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <iostream>
using namespace sandbox;

void check(bool ok) { if (!ok) throw std::runtime_error("control FD contract failed"); }
void exercise(const char *kind, bool attach, bool expected) {
    int sockets[2]; check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    ControlChannel sender{UniqueFd(sockets[0])};
    ControlChannel receiver{UniqueFd(sockets[1])};
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC)); check(static_cast<bool>(fd));
    sender.send(kind, Json::object(), attach ? fd.get() : -1);
    bool accepted = false;
    try {
        auto message = receiver.receive();
        accepted = true;
        check(message.has_value() && static_cast<bool>(message->fd) == attach);
        if (attach) check((::fcntl(message->fd.get(), F_GETFD) & FD_CLOEXEC) != 0);
    } catch (const std::exception &) {
        if (expected) throw;
    }
    check(accepted == expected);
}
int main() {
    exercise("STDIN", true, true);
    exercise("INIT_CREATED", true, true);
    exercise("ENV_READY", false, true);
    exercise("STDIN", false, false);
    exercise("ENV_READY", true, false);
    std::cout << "control descriptor contract passed\n";
}
