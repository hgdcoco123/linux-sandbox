#include "sandbox/environment.hpp"
#include "sandbox/isolation.hpp"
#include "sandbox/protocol.hpp"
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <linux/sched.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace sandbox;
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--probe-environment")
        return launcher_environment_probe();
    if (argc != 1)
        return 2;
    ControlChannel parent{UniqueFd(3)};
    pid_t init_pid = -1;
    UniqueFd init_ref;
    bool waited = false;
    try {
        // All startup data arrives through inherited descriptors, never caller-controlled argv
        // roles.
        std::string data;
        char b[4096];
        for (;;) {
            auto n = ::read(4, b, sizeof(b));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                system_error("read bootstrap");
            if (!n)
                break;
            data.append(b, static_cast<std::size_t>(n));
            if (data.size() > 1024 * 1024)
                throw std::runtime_error("bootstrap too large");
        }
        ::close(4);
        auto j = Json::parse(data);
        auto config = RunConfig::parse(j.at("config"), "/");
        auto root = fs::path(j.at("root_mountpoint").get<std::string>());
        // Recheck structural constraints; parent already checks paths and process-mode opt-in.
        config.validate(config.mode == "process");
        int pidfd = -1;
        clone_args args{};
        args.flags = CLONE_PIDFD;
        args.pidfd = reinterpret_cast<std::uint64_t>(&pidfd);
        args.exit_signal = SIGCHLD;
        if (config.mode == "isolated") {
            args.flags |= CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS;
            if (config.network == "none")
                args.flags |= CLONE_NEWNET;
        }
        init_pid = static_cast<pid_t>(::syscall(SYS_clone3, &args, sizeof(args)));
        checked(init_pid, "clone3 init");
        if (init_pid == 0) {
            ::close(3); // P2 must not keep P1's parent connection alive.
            int code = init_main(config, root);
            ::_exit(code);
        }
        init_ref.reset(pidfd);
        ::close(5); // P1 never participates in the P0/P2 protocol.
        ::close(0);
        ::close(1);
        ::close(2); // Otherwise target output EOF can never arrive.
        if (init_ref.get() != 4) {
            checked(::dup3(init_ref.get(), 4, O_CLOEXEC), "preserve init pidfd");
            init_ref.reset();
            init_ref = UniqueFd(4);
        }
        close_from(6);
        parent.send("INIT_CREATED", {{"pid", init_pid}}, init_ref.get());
        for (;;) {
            int status;
            auto n = ::waitpid(init_pid, &status, WNOHANG);
            if (n == init_pid) {
                waited = true;
                parent.send("INIT_REAPED", {{"wait_status", status}});
                return 0;
            }
            if (n < 0 && errno != EINTR)
                system_error("wait init");
            pollfd fd{parent.fd(), POLLIN | POLLHUP, 0};
            int ready = ::poll(&fd, 1, 20);
            if (ready < 0 && errno != EINTR)
                system_error("poll parent");
            if (ready > 0) {
                auto message = parent.receive();
                if (message)
                    throw std::runtime_error("unexpected parent-to-launcher message");
            }
        }
    } catch (const std::exception &e) {
        try {
            parent.send("BOOTSTRAP_ERROR", {{"error", e.what()}});
        } catch (...) {
        }
        if (init_pid > 0 && !waited) {
            if (init_ref) {
                try {
                    send_pidfd_signal(init_ref.get(), SIGKILL);
                } catch (...) {
                }
            } else
                ::kill(init_pid, SIGKILL);
            while (::waitpid(init_pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        return 125;
    }
}
