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
Message receive_kind(ControlChannel &c, const char *kind) {
    auto m = c.receive();
    if (!m || m->body.at("kind") != kind)
        throw std::runtime_error("unexpected session frame");
    return std::move(*m);
}
} // namespace
int persistent_init(const RunConfig &config, const fs::path &root) {
    ControlChannel control{UniqueFd(5)};
    bool isolated = config.mode == "isolated";
    try {
        checked(::prctl(PR_SET_PDEATHSIG, SIGKILL), "session parent death");
        if (!isolated)
            checked(::prctl(PR_SET_CHILD_SUBREAPER, 1), "session subreaper");
        auto prepare = control.receive();
        if (prepare && prepare->body.at("kind") == "STOP") {
            control.send("INIT_DONE");
            return 0;
        }
        if (!prepare || prepare->body.at("kind") != "PREPARE")
            throw std::runtime_error("expected PREPARE");
        if (isolated)
            RootfsBuilder::build(config, root);
        else
            close_from(6);
        SecurityPolicy::drop_capabilities();
        checked(::prctl(PR_SET_DUMPABLE, 0), "session dumpable");
        checked(::prctl(PR_SET_PDEATHSIG, SIGKILL), "session parent death after setup");
        ::close(0);
        ::close(1);
        ::close(2);
        control.send("ENV_READY");
        for (;;) {
            auto frame = control.receive();
            if (!frame)
                continue;
            auto kind = frame->body.at("kind").get<std::string>();
            if (kind == "STOP") {
                control.send("INIT_DONE");
                return 0;
            }
            if (kind == "CANCEL")
                continue; // The command may have completed before cancellation arrived.
            if (kind != "STDIN" || !frame->fd)
                throw std::runtime_error("expected STDIN fd");
            // Keep all sources above the dup2 destination range.
            UniqueFd input(checked(::fcntl(frame->fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdin"));
            frame->fd.reset();
            auto out = receive_kind(control, "STDOUT");
            auto err = receive_kind(control, "STDERR");
            if (!out.fd || !err.fd)
                throw std::runtime_error("missing output descriptors");
            UniqueFd output(checked(::fcntl(out.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdout"));
            UniqueFd error(checked(::fcntl(err.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stderr"));
            auto exec = receive_kind(control, "COMMAND");
            auto spec = exec.body.at("payload");
            auto cfg = RunConfig::parse(spec.at("config"), "/");
            auto id = spec.at("command_id");
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
            Json result = {{"command_id", id},
                           {"exit_code", nullptr},
                           {"signal", nullptr},
                           {"exec_errno", nullptr}};
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
                        auto k = m->body.at("kind");
                        if (k == "STOP") {
                            closing = true;
                            stop("session_closed");
                        } else if (k == "CANCEL")
                            stop("cancelled");
                        else
                            throw std::runtime_error("invalid active command frame");
                    }
                }
                if (!observed && fds[1].revents) {
                    int e = 0;
                    auto count = ::read(exec_error.read.get(), &e, sizeof(e));
                    if (count == sizeof(e)) {
                        result["exec_errno"] = e;
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
                                result["exit_code"] = WEXITSTATUS(status);
                            if (WIFSIGNALED(status))
                                result["signal"] = WTERMSIG(status);
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
                    result["stop_reason"] = reason;
                    result["execution_ms"] = now - start;
                    control.send("COMMAND_DONE", result);
                    break;
                }
            }
            if (closing) {
                control.send("INIT_DONE");
                return 0;
            }
        }
    } catch (const std::exception &e) {
        try {
            control.send("INIT_ERROR", {{"error", e.what()}});
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
