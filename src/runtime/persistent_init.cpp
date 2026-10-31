#include "sandbox/init_common.hpp"
#include "sandbox/isolation.hpp"
#include "sandbox/protocol.hpp"
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
namespace sandbox {
namespace {
void signal_children(int sig, bool isolated) {
    if (isolated) {
        if (::kill(-1, sig) < 0 && errno != ESRCH)
            system_error("session signal");
    } else {
        std::ifstream in("/proc/self/task/" + std::to_string(::getpid()) + "/children");
        if (!in)
            throw std::runtime_error("cannot enumerate children");
        pid_t pid;
        while (in >> pid)
            if (::kill(pid, sig) < 0 && errno != ESRCH)
                system_error("session signal");
    }
}
ControlEnvelope receive_kind(ControlChannel &c, ControlKind kind) {
    auto envelope = c.receive();
    if (!envelope || sandbox::kind_of(envelope->message) != kind)
        throw std::runtime_error("unexpected session frame");
    return std::move(*envelope);
}

CommandStop stop_code(const std::string &reason) {
    if (reason == "cancelled")
        return CommandStop::cancelled;
    if (reason == "timeout")
        return CommandStop::timeout;
    if (reason == "session_closed")
        return CommandStop::session_closed;
    if (reason == "startup_failure")
        return CommandStop::startup_failure;
    if (reason == "host_error")
        return CommandStop::supervision_failure;
    return CommandStop::main_exit;
}
} // namespace
int persistent_init(const RunConfig &config, const fs::path &root) {
    ControlChannel control{UniqueFd(5)};
    bool isolated = config.mode == "isolated";
    try {
        arm_init(isolated);
        auto prepare = control.receive();
        if (prepare && std::holds_alternative<Stop>(prepare->message)) {
            control.send(InitDone{});
            return 0;
        }
        if (!prepare || sandbox::kind_of(prepare->message) != ControlKind::prepare)
            throw std::runtime_error("expected PREPARE");
        prepare_environment(config, root);
        ::close(0);
        ::close(1);
        ::close(2);
        control.send(EnvReady{});
        for (;;) {
            auto frame = control.receive();
            if (!frame)
                continue;
            if (std::holds_alternative<Stop>(frame->message)) {
                control.send(InitDone{});
                return 0;
            }
            if (std::holds_alternative<CancelCommand>(frame->message))
                continue; // The command may have completed before cancellation arrived.
            if (!std::holds_alternative<StdinHandle>(frame->message) || !frame->fd)
                throw std::runtime_error("expected STDIN fd");
            // Keep all sources above the dup2 destination range.
            UniqueFd input(checked(::fcntl(frame->fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdin"));
            frame->fd.reset();
            auto out = receive_kind(control, ControlKind::stdout_handle);
            auto err = receive_kind(control, ControlKind::stderr_handle);
            if (!out.fd || !err.fd)
                throw std::runtime_error("missing output descriptors");
            UniqueFd output(checked(::fcntl(out.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdout"));
            UniqueFd error(checked(::fcntl(err.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stderr"));
            auto exec = receive_kind(control, ControlKind::command);
            auto *command = std::get_if<Command>(&exec.message);
            if (!command)
                throw std::runtime_error("expected command");
            auto cfg = RunConfig::parse(Json::parse(command->config_blob), "/");
            const CommandIndex index = command->command;
            std::vector<char *> argv;
            for (auto &a : cfg.argv)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            std::vector<std::string> strings;
            for (auto &[k, v] : cfg.env)
                strings.push_back(k + "=" + v);
            std::vector<char *> env;
            for (auto &v : strings)
                env.push_back(v.data());
            env.push_back(nullptr);
            auto exec_error = Pipe::create();
            pid_t pid = ::fork();
            checked(pid, "session fork command");
            if (!pid) {
                int efd = exec_error.write.get();
                auto fail = [&](int number) {
                    ::write(efd, &number, sizeof(number));
                    ::_exit(127);
                };
                if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || (isolated && ::getppid() != 1))
                    fail(errno ? errno : ESRCH);
                if (::setpgid(0, 0) < 0 || ::chdir(cfg.cwd.c_str()) < 0)
                    fail(errno);
                if (::dup2(input.get(), 0) < 0 || ::dup2(output.get(), 1) < 0 ||
                    ::dup2(error.get(), 2) < 0)
                    fail(errno);
                if (efd != 3 && ::dup3(efd, 3, O_CLOEXEC) < 0)
                    fail(errno);
                efd = 3;
                close_from(4);
                try {
                    SecurityPolicy::reset_target_signals();
                    SecurityPolicy::install_baseline();
                } catch (...) {
                    fail(EPERM);
                }
                ::execve(argv[0], argv.data(), env.data());
                fail(errno);
            }
            input.reset();
            output.reset();
            error.reset();
            out.fd.reset();
            err.fd.reset();
            exec_error.write.reset();
            nonblocking(exec_error.read.get());
            bool stopping = false, closing = false, main_done = false, observed = false;
            auto start = monotonic_ms(), stop_at = start;
            std::string reason;
            CommandDone result{index};
            auto stop = [&](const std::string &why) {
                if (!stopping) {
                    stopping = true;
                    stop_at = monotonic_ms();
                    reason = why;
                }
            };
            for (;;) {
                pollfd fds[2]{{control.fd(), POLLIN | POLLHUP, 0},
                              {exec_error.read.get(), POLLIN | POLLHUP, 0}};
                int n = ::poll(fds, 2, 20);
                if (n < 0 && errno == EINTR)
                    continue;
                checked(n, "command poll");
                if (fds[0].revents) {
                    auto m = control.receive(true);
                    if (m) {
                        if (std::holds_alternative<Stop>(m->message)) {
                            closing = true;
                            stop("session_closed");
                        } else if (std::holds_alternative<CancelCommand>(m->message))
                            stop("cancelled");
                        else
                            throw std::runtime_error("invalid active command frame");
                    }
                }
                if (!observed && fds[1].revents) {
                    int e = 0;
                    auto count = ::read(exec_error.read.get(), &e, sizeof(e));
                    if (count == sizeof(e)) {
                        result.exec_errno = e;
                        stop("startup_failure");
                        observed = true;
                        exec_error.read.reset();
                    } else if (count == 0) {
                        observed = true;
                        exec_error.read.reset();
                    } else if (count > 0)
                        throw std::runtime_error("partial exec failure");
                }
                bool empty = false;
                for (;;) {
                    int status;
                    auto child = ::waitpid(-1, &status, WNOHANG);
                    if (child > 0) {
                        if (child == pid) {
                            main_done = true;
                            if (WIFEXITED(status))
                                result.exit_code = WEXITSTATUS(status);
                            if (WIFSIGNALED(status))
                                result.signal = WTERMSIG(status);
                            stop("main_exited");
                        }
                        continue;
                    }
                    if (child < 0 && errno == EINTR)
                        continue;
                    if (child < 0 && errno != ECHILD)
                        system_error("command wait");
                    empty = child < 0 && errno == ECHILD;
                    break;
                }
                auto now = monotonic_ms();
                if (!stopping && now - start >= cfg.limits.wall_ms)
                    stop("timeout");
                if (stopping)
                    signal_children(now - stop_at >= cfg.limits.grace_ms ? SIGKILL : SIGTERM,
                                    isolated);
                if (stopping && now - stop_at > cfg.limits.grace_ms + 4000)
                    throw std::runtime_error("command descendants cleanup deadline");
                if (main_done && empty && observed) {
                    result.stop = stop_code(reason);
                    result.elapsed = std::chrono::milliseconds(static_cast<std::int64_t>(now - start));
                    control.send(result);
                    break;
                }
            }
            if (closing) {
                control.send(InitDone{});
                return 0;
            }
        }
    } catch (const std::exception &e) {
        try {
            control.send(InitError{e.what()});
        } catch (...) {
        }
        try {
            signal_children(SIGKILL, isolated);
        } catch (...) {
        }
        // Do not wait forever for an uninterruptible child; PID namespace exit kills remaining
        // tasks.
        return 125;
    }
}
} // namespace sandbox
