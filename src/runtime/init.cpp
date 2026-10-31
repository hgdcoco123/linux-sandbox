#include "sandbox/init_common.hpp"
#include "sandbox/isolation.hpp"
#include "sandbox/protocol.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>
namespace sandbox {
namespace {
struct StartupCancelled {};
struct ExecFailure {
    int stage, error;
};
[[noreturn]] void fail_exec(int fd, int stage, int error) {
    ExecFailure e{stage, error};
    const char *p = reinterpret_cast<const char *>(&e);
    std::size_t left = sizeof(e);
    while (left) {
        auto n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    ::_exit(127);
}
void children_signal(int signal, bool isolated) {
    if (isolated) {
        if (::kill(-1, signal) < 0 && errno != ESRCH)
            system_error("namespace signal");
        return;
    }
    // Diagnostic backend: never broadcast kill(-1) in host PID namespace.
    std::ifstream in("/proc/self/task/" + std::to_string(::getpid()) + "/children");
    if (!in)
        throw std::runtime_error("read adopted children");
    pid_t pid;
    while (in >> pid)
        if (::kill(pid, signal) < 0 && errno != ESRCH)
            system_error("signal adopted child");
}
ControlEnvelope expect(ControlChannel &channel, ControlKind kind) {
    auto envelope = channel.receive();
    if (!envelope)
        throw std::runtime_error("control channel closed during preparation");
    if (std::holds_alternative<Stop>(envelope->message))
        throw StartupCancelled{};
    if (sandbox::kind_of(envelope->message) != kind)
        throw std::runtime_error(std::string("expected ") + control_kind_name(kind));
    return std::move(*envelope);
}
} // namespace
int persistent_init(const RunConfig &, const fs::path &);
int init_main(const RunConfig &config, const fs::path &root) {
    if (config.persistent)
        return persistent_init(config, root);
    ControlChannel control{UniqueFd(5)};
    pid_t main_pid = -1;
    bool main_reaped = false;
    bool isolated = config.mode == "isolated";
    try {
        arm_init(isolated);
        expect(control, ControlKind::prepare);
        prepare_environment(config, root);
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGCHLD);
        checked(::sigprocmask(SIG_BLOCK, &mask, nullptr), "block init SIGCHLD");
        UniqueFd signals(
            checked(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK), "init signalfd"));
        control.send(EnvReady{});
        expect(control, ControlKind::exec);
        auto execpipe = Pipe::create();
        std::vector<char *> argv;
        for (const auto &s : config.argv)
            argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        std::vector<std::string> environment;
        for (const auto &[k, v] : config.env)
            environment.push_back(k + "=" + v);
        std::vector<char *> env;
        for (auto &s : environment)
            env.push_back(s.data());
        env.push_back(nullptr);
        main_pid = ::fork();
        checked(main_pid, "fork target");
        if (main_pid == 0) {
            int errorfd = execpipe.write.get();
            try {
                if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
                    fail_exec(errorfd, 1, errno);
                if (::getppid() != 1 && isolated)
                    ::_exit(127);
                if (::setpgid(0, 0) < 0)
                    fail_exec(errorfd, 2, errno);
                if (::chdir(config.cwd.c_str()) < 0)
                    fail_exec(errorfd, 3, errno);
                SecurityPolicy::reset_target_signals();
                if (errorfd != 3 && ::dup3(errorfd, 3, O_CLOEXEC) < 0)
                    fail_exec(errorfd, 4, errno);
                errorfd = 3;
                close_from(4);
                SecurityPolicy::install_baseline();
                ::execve(argv.front(), argv.data(), env.data());
                fail_exec(3, 6, errno);
            } catch (...) {
                fail_exec(errorfd, 5, errno ? errno : EPERM);
            }
        }
        execpipe.write.reset();
        nonblocking(execpipe.read.get());
        // init must not keep target output writers alive.
        ::close(0);
        ::close(1);
        ::close(2);
        std::array<char, sizeof(ExecFailure)> bytes{};
        std::size_t size = 0;
        bool exec_known = false, exec_failed = false, stopping = false, control_alive = true;
        std::uint64_t stop_at = 0;
        auto stop = [&]() {
            if (!stopping) {
                stopping = true;
                stop_at = monotonic_ms();
            }
            children_signal(SIGTERM, isolated);
        };
        auto notify = [&](const ControlMessage &message) {
            if (control_alive)
                control.send(message);
        };
        for (;;) {
            pollfd fds[3]{{control.fd(), POLLIN | POLLHUP, 0},
                          {signals.get(), POLLIN, 0},
                          {execpipe.read.get(), POLLIN | POLLHUP, 0}};
            int n = ::poll(fds, 3, 20);
            if (n < 0 && errno == EINTR)
                continue;
            checked(n, "init poll");
            if (fds[0].revents && control_alive) {
                try {
                    auto message = control.receive(true);
                    if (message) {
                        if (!std::holds_alternative<Stop>(message->message))
                            throw std::runtime_error("invalid init command");
                        stop();
                    }
                } catch (...) {
                    control_alive = false;
                    control.close();
                    stop();
                }
            }
            if (fds[1].revents) {
                signalfd_siginfo info;
                while (::read(signals.get(), &info, sizeof(info)) > 0) {
                }
            }
            if (fds[2].revents && execpipe.read) {
                while (execpipe.read) {
                    if (size == bytes.size()) {
                        ExecFailure e;
                        std::memcpy(&e, bytes.data(), sizeof(e));
                        exec_failed = true;
                        exec_known = true;
                        notify(StartFailed{e.stage, e.error});
                        execpipe.read.reset();
                        break;
                    }
                    auto count =
                        ::read(execpipe.read.get(), bytes.data() + size, bytes.size() - size);
                    if (count > 0) {
                        size += static_cast<std::size_t>(count);
                        continue;
                    }
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    if (count < 0)
                        system_error("init exec error pipe");
                    exec_known = true;
                    execpipe.read.reset();
                    if (size)
                        throw std::runtime_error("partial exec failure");
                    notify(ExecObserved{});
                }
            }
            bool empty = false;
            for (;;) {
                int status;
                pid_t child = ::waitpid(-1, &status, WNOHANG);
                if (child > 0) {
                    if (child == main_pid) {
                        main_reaped = true;
                        MainExit exit;
                        if (WIFEXITED(status))
                            exit.exit_code = WEXITSTATUS(status);
                        if (WIFSIGNALED(status))
                            exit.signal = WTERMSIG(status);
                        notify(exit);
                        stop();
                    }
                    continue;
                }
                if (child < 0 && errno == EINTR)
                    continue;
                if (child < 0 && errno != ECHILD)
                    system_error("init waitpid");
                empty = child < 0 && errno == ECHILD;
                break;
            }
            if (exec_failed && !stopping)
                stop();
            if (stopping) {
                children_signal(monotonic_ms() - stop_at >= config.limits.grace_ms ? SIGKILL
                                                                                   : SIGTERM,
                                isolated);
            }
            if (main_reaped && empty && exec_known) {
                notify(InitDone{});
                return 0;
            }
        }
    } catch (const StartupCancelled &) {
        try {
            control.send(InitDone{});
        } catch (...) {
        }
        return 0;
    } catch (const std::exception &e) {
        try {
            control.send(InitError{e.what()});
        } catch (...) {
        }
        if (main_pid > 0 && !main_reaped) {
            try {
                children_signal(SIGKILL, isolated);
            } catch (...) {
            }
            while (::waitpid(-1, nullptr, 0) > 0) {
            }
        }
        return 125;
    }
}
} // namespace sandbox
