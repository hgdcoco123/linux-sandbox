#include "sandbox/init_common.hpp"
#include "sandbox/interactive.hpp"
#include "sandbox/isolation.hpp"
#include "sandbox/protocol.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sandbox {
namespace {

struct StartupCancelled {};

// The child side of a failed exec: report where it stopped, then die. Written as
// one fixed-size record so the parent can tell "partial write" from "no failure".
[[noreturn]] void fail_exec(int fd, int stage, int error) {
    ExecFailure failure{stage, error};
    const char *bytes = reinterpret_cast<const char *>(&failure);
    std::size_t left = sizeof(failure);
    while (left) {
        auto written = ::write(fd, bytes, left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        bytes += written;
        left -= static_cast<std::size_t>(written);
    }
    ::_exit(127);
}

// Signal every process init is responsible for. In the isolated backend init is a
// pid namespace's pid 1, so kill(-1) is scoped by the kernel; the diagnostic
// backend shares the host pid namespace, where kill(-1) would be catastrophic.
void signal_children(int signal, bool isolated) {
    if (isolated) {
        if (::kill(-1, signal) < 0 && errno != ESRCH)
            system_error("namespace signal");
        return;
    }
    std::ifstream in("/proc/self/task/" + std::to_string(::getpid()) + "/children");
    if (!in)
        throw std::runtime_error("cannot enumerate children");
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

struct Spawned {
    pid_t pid = -1;
    // Read end of the exec-error pipe. Closed by the target on a successful exec.
    UniqueFd exec_error;
};

// Fork the target and exec it. A descriptor argument of -1 means "keep what init
// inherited" (the launcher already wired the one-shot target's stdio).
Spawned spawn_target(const RunConfig &config, int stdin_fd, int stdout_fd, int stderr_fd,
                     bool isolated) {
    std::vector<char *> argv;
    for (const auto &argument : config.argv)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> strings;
    for (const auto &[name, value] : config.env)
        strings.push_back(name + "=" + value);
    std::vector<char *> env;
    for (auto &entry : strings)
        env.push_back(entry.data());
    env.push_back(nullptr);

    auto exec_error = Pipe::create();
    pid_t pid = ::fork();
    checked(pid, "fork target");
    if (pid != 0) {
        exec_error.write.reset();
        nonblocking(exec_error.read.get());
        return Spawned{pid, std::move(exec_error.read)};
    }
    const int errorfd = exec_error.write.get();
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
        fail_exec(errorfd, exec_stage_pdeathsig, errno);
    // A pid 1 that was re-parented is no longer supervised by anyone.
    if (isolated && ::getppid() != 1)
        fail_exec(errorfd, exec_stage_pdeathsig, ECHILD);
    if (::setpgid(0, 0) < 0)
        fail_exec(errorfd, exec_stage_setpgid, errno);
    if (::chdir(config.cwd.c_str()) < 0)
        fail_exec(errorfd, exec_stage_chdir, errno);
    if (stdin_fd >= 0 && (::dup2(stdin_fd, 0) < 0 || ::dup2(stdout_fd, 1) < 0 ||
                          ::dup2(stderr_fd, 2) < 0))
        fail_exec(errorfd, exec_stage_descriptors, errno);
    if (errorfd != 3 && ::dup3(errorfd, 3, O_CLOEXEC) < 0)
        fail_exec(errorfd, exec_stage_descriptors, errno);
    close_from(4);
    try {
        SecurityPolicy::reset_target_signals();
        SecurityPolicy::install_baseline();
    } catch (...) {
        fail_exec(3, exec_stage_security, errno ? errno : EPERM);
    }
    ::execve(argv.front(), argv.data(), env.data());
    fail_exec(3, exec_stage_execve, errno);
}

// The wall deadline may be enforced here (sessions: init owns the command budget)
// or by the supervisor (one-shot: it already owns the stop ladder).
struct SupervisionRules {
    std::uint64_t wall_ms = 0;
    std::uint64_t grace_ms = 500;
    std::uint64_t close_deadline_ms = 0;
};

// Facts the caller wants as they happen, not at the end: the supervisor on the
// other side of the channel starts draining output on MAIN_EXIT.
struct CommandWatcher {
    std::function<void(int exit_code, int signal)> main_exited;
    std::function<void(int stage, int error)> start_failed;
    // The exec succeeded. The supervisor distinguishes "the target ran" from
    // "the environment came up", and only this fact says the former.
    std::function<void()> exec_observed;
};

struct CommandOutcome {
    std::optional<int> exit_code, signal, exec_errno;
    CommandStop stop = CommandStop::main_exit;
    bool session_close_requested = false;
    std::chrono::milliseconds elapsed{0};
};

// Supervise one target until it and every descendant are gone. Shared by both
// modes: the only difference is what they do with the outcome.
CommandOutcome supervise_command(ControlChannel &control, const SupervisionRules &rules, pid_t pid,
                                 UniqueFd exec_error, bool isolated,
                                 const CommandWatcher &watcher) {
    auto started = monotonic_ms();
    bool stopping = false, main_done = false, exec_resolved = false;
    auto stop_at = started;
    CommandOutcome outcome;
    auto stop = [&](CommandStop reason) {
        if (stopping)
            return; // first reason wins
        stopping = true;
        stop_at = monotonic_ms();
        outcome.stop = reason;
    };
    for (;;) {
        pollfd fds[2]{{control.fd(), POLLIN | POLLHUP, 0},
                      {exec_error.get(), POLLIN | POLLHUP, 0}};
        int ready = ::poll(fds, 2, 20);
        if (ready < 0 && errno == EINTR)
            continue;
        checked(ready, "init poll");
        if (fds[0].revents) {
            auto message = control.receive(true);
            if (message) {
                if (std::holds_alternative<Stop>(message->message)) {
                    outcome.session_close_requested = true;
                    stop(CommandStop::session_closed);
                } else if (std::holds_alternative<CancelCommand>(message->message)) {
                    // The command may have completed before this arrived.
                    stop(CommandStop::cancelled);
                } else {
                    throw std::runtime_error("invalid active command frame");
                }
            }
        }
        if (!exec_resolved && exec_error && fds[1].revents) {
            std::array<char, sizeof(ExecFailure)> bytes{};
            std::size_t size = 0;
            bool resolved = false;
            while (exec_error) {
                if (size == bytes.size()) {
                    ExecFailure failure;
                    std::memcpy(&failure, bytes.data(), sizeof(failure));
                    outcome.exec_errno = failure.error;
                    stop(CommandStop::startup_failure);
                    if (watcher.start_failed)
                        watcher.start_failed(failure.stage, failure.error);
                    resolved = true;
                    break;
                }
                auto count = ::read(exec_error.get(), bytes.data() + size, bytes.size() - size);
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
                // EOF: a closed pipe with a partial record is not "exec succeeded".
                if (size)
                    throw std::runtime_error("partial exec failure");
                resolved = true;
                if (watcher.exec_observed)
                    watcher.exec_observed();
                break;
            }
            if (resolved) {
                exec_resolved = true;
                exec_error.reset();
            }
        }
        bool empty = false;
        for (;;) {
            int status = 0;
            auto child = ::waitpid(-1, &status, WNOHANG);
            if (child > 0) {
                if (child == pid) {
                    main_done = true;
                    if (WIFEXITED(status))
                        outcome.exit_code = WEXITSTATUS(status);
                    if (WIFSIGNALED(status))
                        outcome.signal = WTERMSIG(status);
                    if (watcher.main_exited)
                        watcher.main_exited(outcome.exit_code.value_or(-1),
                                            outcome.signal.value_or(0));
                    stop(CommandStop::main_exit);
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
        auto now = monotonic_ms();
        if (!stopping && rules.wall_ms && now - started >= rules.wall_ms)
            stop(CommandStop::timeout);
        if (stopping)
            signal_children(now - stop_at >= rules.grace_ms ? SIGKILL : SIGTERM, isolated);
        if (stopping && rules.close_deadline_ms &&
            now - stop_at > rules.close_deadline_ms)
            throw std::runtime_error("command descendants cleanup deadline");
        if (main_done && empty && exec_resolved) {
            outcome.elapsed = std::chrono::milliseconds(static_cast<std::int64_t>(started ? now - started : 0));
            return outcome;
        }
    }
}

// One command, then done. The target's stdio was wired by the launcher; init only
// has to stop holding the write ends so the pipes can reach EOF.
int run_once(ControlChannel &control, const RunConfig &config, const fs::path &root) {
    const bool isolated = config.mode == "isolated";
    arm_init(isolated);
    expect(control, ControlKind::prepare);
    prepare_environment(config, root);
    control.send(EnvReady{});
    expect(control, ControlKind::exec);
    SupervisionRules rules;
    rules.grace_ms = config.limits.grace_ms;
    const CommandWatcher watcher{
        [&](int exit_code, int signal) {
            MainExit message;
            if (exit_code >= 0)
                message.exit_code = exit_code;
            if (signal > 0)
                message.signal = signal;
            try {
                control.send(message);
            } catch (...) {
                // The supervisor is gone; the supervisor's own deadline reports it.
            }
        },
        [&](int stage, int error) {
            try {
                control.send(StartFailed{stage, error});
            } catch (...) {
            }
        },
        [&] {
            try {
                control.send(ExecObserved{});
            } catch (...) {
            }
        }};
    auto spawned = spawn_target(config, -1, -1, -1, isolated);
    // init must not keep the target's output writers alive.
    ::close(0);
    ::close(1);
    ::close(2);
    supervise_command(control, rules, spawned.pid, std::move(spawned.exec_error), isolated,
                      watcher);
    control.send(InitDone{});
    return 0;
}

// A single path component, so a transfer can never address anything outside the
// workspace or the transfer directory.
bool plain_component(const std::string &name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

// Move one uploaded tree into the workspace. The destination must not exist: an
// upload never overwrites what the target already wrote.
Transferred transfer_in(const RunConfig &config, const std::string &name) {
    Transferred answer;
    try {
        if (config.workspace_transfer.empty())
            throw std::runtime_error("workspace transfer is not configured");
        if (!plain_component(name))
            throw std::runtime_error("transfer name is not a single component");
        auto copied = file_request({{"op", "snapshot"},
                                    {"source", (fs::path("/transfer/in") / name).string()},
                                    {"destination", (fs::path("/output") / name).string()}},
                                   config.workspace);
        answer.ok = true;
        answer.bytes = copied.value("bytes", std::uint64_t{0});
        answer.entries = static_cast<std::uint32_t>(copied.at("entries").size());
    } catch (const std::exception &failure) {
        answer.error = failure.what();
    }
    return answer;
}

// Copy the whole workspace out under a tag that must not exist yet, so a failed
// export can never damage the previous good copy.
Transferred transfer_out(const RunConfig &config, const std::string &tag) {
    Transferred answer;
    try {
        if (config.workspace_transfer.empty())
            throw std::runtime_error("workspace transfer is not configured");
        if (!plain_component(tag))
            throw std::runtime_error("transfer tag is not a single component");
        auto copied = file_request({{"op", "snapshot"},
                                    {"source", "/output"},
                                    {"destination", (fs::path("/transfer") / tag).string()}},
                                   config.workspace);
        answer.ok = true;
        answer.bytes = copied.value("bytes", std::uint64_t{0});
        answer.entries = static_cast<std::uint32_t>(copied.at("entries").size());
    } catch (const std::exception &failure) {
        answer.error = failure.what();
    }
    return answer;
}

// One environment, many commands. init builds the environment once and supervises
// each command with the same code the one-shot mode uses.
int run_persistent(ControlChannel &control, const RunConfig &config, const fs::path &root) {
    const bool isolated = config.mode == "isolated";
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
            continue; // no command is active: nothing to cancel
        if (auto *import = std::get_if<ImportTree>(&frame->message)) {
            control.send(transfer_in(config, import->name));
            continue;
        }
        if (auto *exported = std::get_if<ExportTree>(&frame->message)) {
            control.send(transfer_out(config, exported->tag));
            continue;
        }
        if (!std::holds_alternative<StdinHandle>(frame->message) || !frame->fd)
            throw std::runtime_error("expected STDIN fd");
        // Keep every source above the dup2 destination range.
        UniqueFd input(checked(::fcntl(frame->fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdin"));
        frame->fd.reset();
        auto out = expect(control, ControlKind::stdout_handle);
        auto err = expect(control, ControlKind::stderr_handle);
        if (!out.fd || !err.fd)
            throw std::runtime_error("missing output descriptors");
        UniqueFd output(checked(::fcntl(out.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stdout"));
        UniqueFd error(checked(::fcntl(err.fd.get(), F_DUPFD_CLOEXEC, 100), "copy stderr"));
        auto exec = expect(control, ControlKind::command);
        const auto *command = std::get_if<Command>(&exec.message);
        if (!command)
            throw std::runtime_error("expected command");
        const auto command_config = RunConfig::parse(Json::parse(command->config_blob), "/");
        const CommandIndex index = command->command;
        SupervisionRules rules;
        rules.wall_ms = command_config.limits.wall_ms;
        rules.grace_ms = command_config.limits.grace_ms;
        rules.close_deadline_ms = command_config.limits.grace_ms + 4000;
        // Only the closing frame can stop a session command; MAIN_EXIT is reported
        // to the supervisor when the command's result is sent, not before.
        const CommandWatcher watcher{};
        auto spawned = spawn_target(command_config, input.get(), output.get(), error.get(),
                                    isolated);
        input.reset();
        output.reset();
        error.reset();
        out.fd.reset();
        err.fd.reset();
        auto outcome = supervise_command(control, rules, spawned.pid,
                                         std::move(spawned.exec_error), isolated, watcher);
        CommandDone done{index};
        done.exit_code = outcome.exit_code;
        done.signal = outcome.signal;
        done.exec_errno = outcome.exec_errno;
        done.stop = outcome.stop;
        done.elapsed = outcome.elapsed;
        // The command's facts come first even when the session is closing: the
        // supervisor needs them to finish its record before it can accept INIT_DONE.
        control.send(done);
        if (outcome.session_close_requested) {
            control.send(InitDone{});
            return 0;
        }
    }
}

} // namespace

int init_main(const RunConfig &config, const fs::path &root) {
    ControlChannel control{UniqueFd(5)};
    const bool isolated = config.mode == "isolated";
    try {
        return config.persistent ? run_persistent(control, config, root)
                                 : run_once(control, config, root);
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
        try {
            signal_children(SIGKILL, isolated);
        } catch (...) {
        }
        // Never wait forever for an uninterruptible child: the pid namespace exit
        // kills whatever is left.
        return 125;
    }
}
} // namespace sandbox
