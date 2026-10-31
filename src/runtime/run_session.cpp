#include "sandbox/core/prepare.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/event_loop.hpp"
#include "sandbox/session.hpp"
#include <fcntl.h>
#include <iostream>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

namespace sandbox {
namespace {
bool result_unknown_or_signaled(const ExecutionOutcome &outcome) {
    return outcome.term_signal().has_value() ||
           std::holds_alternative<Indeterminate>(outcome.variant()) ||
           std::holds_alternative<SupervisionFailed>(outcome.variant());
}

Millis now_ms() {
    return Millis{static_cast<std::int64_t>(monotonic_ms())};
}

SupervisionLimits core_limits(const RunConfig &config) {
    const auto &limits = config.limits;
    const auto &options = config;
    SupervisionLimits core;
    core.startup = Millis{static_cast<std::int64_t>(limits.startup_ms)};
    core.wall = Millis{static_cast<std::int64_t>(limits.wall_ms)};
    // The diagnostic backend has no cgroup to kill: init escalates to SIGKILL on
    // its own after grace_ms, and it needs room to do that before the supervisor
    // takes the init process down. The isolated backend can kill the whole group
    // immediately, so it keeps the plain grace period.
    const auto extra = options.mode == "isolated" ? 0 : 1000;
    core.grace = Millis{static_cast<std::int64_t>(limits.grace_ms) + extra};
    // Drain and settle are supervision allowances, not user policy: they bound
    // how long we keep waiting for facts that should already be on their way.
    core.drain = Millis{5000};
    core.settle = Millis{4000};
    return core;
}

OutputStats stats_of(const StreamReport &report) {
    OutputStats stats;
    stats.saved_bytes = report.saved;
    stats.discarded_bytes = report.discarded;
    stats.failed_bytes = report.failed_bytes;
    stats.queue_peak_bytes = report.queue_peak_bytes;
    stats.backpressure_events = report.backpressure_events;
    stats.write_error = report.write_error;
    return stats;
}

// The report keeps the field's established vocabulary: a natural exit leaves
// stop_reason empty (exit_code/signal already say what happened), and only an
// abnormal end names a reason.
std::string stop_reason_of(const ExecutionOutcome &outcome) {
    if (outcome.exit_code() || outcome.term_signal())
        return {};
    if (std::holds_alternative<TimedOut>(outcome.variant()))
        return "timeout";
    if (std::holds_alternative<Cancelled>(outcome.variant()))
        return "cancelled";
    if (std::holds_alternative<Oom>(outcome.variant()))
        return "oom";
    if (std::holds_alternative<StartupFailed>(outcome.variant()))
        return "startup_failure";
    return "host_error";
}

// Report vocabulary. The outcome is the source of truth; this only names it.
const char *summary_of(const ExecutionOutcome &outcome) {
    if (auto code = outcome.exit_code())
        return *code == 0 ? "success" : "nonzero_exit";
    if (outcome.term_signal())
        return "signaled";
    if (std::holds_alternative<TimedOut>(outcome.variant()))
        return "timeout";
    if (std::holds_alternative<Cancelled>(outcome.variant()))
        return "cancelled";
    if (std::holds_alternative<Oom>(outcome.variant()))
        return "oom";
    if (std::holds_alternative<StartupFailed>(outcome.variant()))
        return "startup_failure";
    return "host_error";
}
} // namespace

RunSession::RunSession(RunConfig c, fs::path root, fs::path launcher)
    : config_(std::move(c)), state_root_(std::move(root)), launcher_path_(std::move(launcher)) {}

Json RunSession::live_metrics() const {
    if (sampler_)
        return {{"metrics", sampler_->live_snapshot()}};
    return {{"metrics",
             {{"available", false},
              {"reason", config_.mode == "process"
                             ? "diagnostic process backend has no per-run cgroup metrics"
                             : "sampler not prepared"}}}};
}

void RunSession::sample_metrics(bool force) {
    if (!sampler_)
        return;
    bool updated = false;
    try {
        updated = sampler_->sample(monotonic_ms(), force);
    } catch (const std::exception &e) {
        if (logger_)
            logger_->warn("metrics sampling failed: {}", e.what());
        return; // Optional observation cannot replace the execution or cancellation policy.
    }
    if (updated && store_)
        store_->state(state_.current(), live_metrics());
}

void RunSession::transition(RunState next) {
    state_.transition(next);
    if (store_)
        store_->state(next, live_metrics());
    if (logger_)
        logger_->info("state={}", state_name(next));
}

void RunSession::mark_stopping() {
    switch (state_.current()) {
    case RunState::preparing:
    case RunState::starting:
    case RunState::running:
        transition(RunState::stopping);
        break;
    default:
        break;
    }
}

void RunSession::prepare() {
    core_ = std::make_unique<ExecutionCore>(core_limits(config_), now_ms());
    store_ = RunStore::create(state_root_, config_);
    report_.id = store_->id();
    report_.isolated = config_.mode == "isolated";
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_st>(
        (store_->directory() / "internal.log").string(), false);
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_st>();
    logger_ = std::make_shared<spdlog::logger>(report_.id, spdlog::sinks_init_list{file, console});
    logger_->set_pattern("[%H:%M:%S] [%n] [%l] %v");
    logger_->flush_on(spdlog::level::warn);
    transition(RunState::preparing);
    logger_->info("run directory: {}", store_->directory().string());
    auto root = store_->directory() / "rootfs";
    checked(::mkdir(root.c_str(), 0700), "create root mountpoint");
    // The state directory is deliberately not undone: the report lives there even
    // when preparation fails. Everything else is journaled and rolled back.
    std::vector<PrepareStepSpec> steps;
    steps.push_back({PrepareStep::control_socket,
                     [this] {
                         control_ = std::make_unique<ControlServer>(store_->directory() /
                                                                    "control.sock");
                     },
                     [this] { control_.reset(); }});
    steps.push_back({PrepareStep::cgroup,
                     [this] {
                         if (config_.mode != "isolated")
                             return;
                         cgroup_ = std::make_unique<CgroupV2>(config_.cgroup_root, store_->id(),
                                                              config_.limits);
                         sampler_ = std::make_unique<MetricsSampler>(
                             cgroup_->path(), MetricRegistry::defaults(),
                             config_.metrics.interval_ms, config_.metrics.history_limit);
                     },
                     [this] {
                         if (!cgroup_)
                             return;
                         try {
                             cgroup_->kill_all();
                             cgroup_->remove();
                         } catch (...) {
                         }
                         cgroup_.reset();
                         sampler_.reset();
                     }});
    steps.push_back({PrepareStep::output_capture,
                     [this] {
                         auto out = Pipe::create(), err = Pipe::create();
                         auto out_file = open_path(store_->directory() / "stdout.bin",
                                                   O_WRONLY | O_CREAT | O_EXCL);
                         auto err_file = open_path(store_->directory() / "stderr.bin",
                                                   O_WRONLY | O_CREAT | O_EXCL);
                         stdout_ = std::make_unique<OutputCapture>(
                             std::move(out.read), std::move(out_file), config_.limits.capture_bytes);
                         stderr_ = std::make_unique<OutputCapture>(
                             std::move(err.read), std::move(err_file), config_.limits.capture_bytes);
                         // Keep the child's write ends until the launcher has them.
                         pipe_out_ = std::move(out.write);
                         pipe_err_ = std::move(err.write);
                     },
                     [this] {
                         stdout_.reset();
                         stderr_.reset();
                         pipe_out_.reset();
                         pipe_err_.reset();
                     }});
    steps.push_back({PrepareStep::launcher,
                     [this, root] {
                         auto in = open_path(
                             config_.stdin_file.empty() ? fs::path("/dev/null") : config_.stdin_file,
                             O_RDONLY);
                         transition(RunState::starting);
                         launcher_ = std::make_unique<LauncherClient>(config_, launcher_path_, root,
                                                                      in.get(), pipe_out_.get(),
                                                                      pipe_err_.get());
                         // Keeping the write ends here would prevent EOF; the launcher
                         // owns them from now on.
                         pipe_out_.reset();
                         pipe_err_.reset();
                     },
                     [this] {
                         if (launcher_) {
                             try {
                                 launcher_->abort();
                             } catch (...) {
                             }
                         }
                         launcher_.reset();
                     }});
    PrepareSequence sequence{std::move(steps)};
    auto outcome = sequence.run();
    if (!outcome.ok) {
        const auto detail = describe(outcome);
        report_.errors.push_back(detail);
        apply(core_->prepare_failed(detail, now_ms()));
        throw std::runtime_error(detail);
    }
}

void RunSession::apply(const std::vector<SupervisionAction> &actions) {
    for (const auto &action : actions) {
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, SignalTarget>) {
                    if (launcher_ && init_created_ && init_channel_open_) {
                        try {
                            launcher_->init_channel().send(Stop{});
                        } catch (const std::exception &e) {
                            if (logger_)
                                logger_->warn("stop channel: {}", e.what());
                        }
                    }
                } else if constexpr (std::is_same_v<T, KillTarget>) {
                    // The group is the only scope that cannot be escaped by forking.
                    try {
                        if (cgroup_)
                            cgroup_->kill_all();
                        else if (launcher_)
                            send_pidfd_signal(launcher_->init_pidfd(), SIGKILL);
                    } catch (const std::exception &e) {
                        report_.cleanup_errors.push_back(std::string("kill: ") + e.what());
                    }
                } else if constexpr (std::is_same_v<T, ForceDrain>) {
                    // Stop accepting new output so the writers can finish.
                    if (stdout_)
                        stdout_->close_incomplete();
                    if (stderr_)
                        stderr_->close_incomplete();
                }
            },
            action);
    }
    // Any phase past running means the execution is winding down, and the report
    // state machine must follow it into "stopping" before finalize().
    if (core_ && core_->phase() != Phase::running && core_->phase() != Phase::starting &&
        core_->phase() != Phase::idle)
        mark_stopping();
}

void RunSession::request_cancel() {
    if (!core_ || core_->finished())
        return;
    apply(core_->stop_requested(StopIntent::cancel, now_ms()));
}

void RunSession::observe_streams(Millis now) {
    if (stdout_) {
        const auto summary = stdout_->summary();
        if (!out_eof_seen_ && summary.eof) {
            out_eof_seen_ = true;
            apply(core_->output_eof(false, now));
        }
        if (!out_done_seen_ && stdout_->complete()) {
            out_done_seen_ = true;
            apply(core_->output_finished(false, stats_of(summary), now));
        }
    }
    if (stderr_) {
        const auto summary = stderr_->summary();
        if (!err_eof_seen_ && summary.eof) {
            err_eof_seen_ = true;
            apply(core_->output_eof(true, now));
        }
        if (!err_done_seen_ && stderr_->complete()) {
            err_done_seen_ = true;
            apply(core_->output_finished(true, stats_of(summary), now));
        }
    }
}

void RunSession::on_launcher(ControlEnvelope message) {
    if (auto *created = std::get_if<InitCreated>(&message.message)) {
        if (init_created_ || !message.fd || !core_)
            throw std::runtime_error("unexpected init identity");
        auto pid = created->pid;
        launcher_->set_init_identity(pid, std::move(message.fd));
        init_created_ = true;
        store_->owner_update({{"init_pid", pid}, {"init_start", process_start(pid)}});
        if (config_.mode == "isolated") {
            auto proc = fs::path("/proc") / std::to_string(pid);
            write_control_file(proc / "setgroups", "deny");
            write_control_file(proc / "uid_map", "0 " + std::to_string(::getuid()) + " 1");
            write_control_file(proc / "gid_map", "0 " + std::to_string(::getgid()) + " 1");
            cgroup_->attach(pid);
            sample_metrics(true); // Establish the baseline before the target can execute.
        }
        const bool stopping = core_->phase() == Phase::stopping || core_->finished();
        launcher_->init_channel().send(stopping ? ControlMessage{Stop{}} : ControlMessage{Prepare{}});
        logger_->info("init created and resources attached, pid={}", pid);
    } else if (auto *reaped = std::get_if<InitReaped>(&message.message)) {
        if (!init_created_ || init_reaped_)
            throw std::runtime_error("unexpected init reap");
        init_reaped_ = true;
        int status = reaped->wait_status;
        init_abnormal_ = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    } else if (auto *error = std::get_if<BootstrapError>(&message.message))
        throw std::runtime_error("launcher: " + error->message);
    else
        throw std::runtime_error("unknown launcher event");
}

void RunSession::on_init(ControlEnvelope message) {
    const Millis now = now_ms();
    if (std::holds_alternative<EnvReady>(message.message)) {
        if (!init_created_ || env_ready_)
            throw std::runtime_error("unexpected ENV_READY");
        env_ready_ = true;
        report_.runtime_prepared = true;
        apply(core_->env_ready(now));
        if (core_->phase() != Phase::running)
            return; // STOP is already queued; never exec after a cancellation.
        launcher_->init_channel().send(Exec{});
        transition(RunState::running);
    } else if (std::holds_alternative<ExecObserved>(message.message)) {
        if (!env_ready_ || report_.exec_observed)
            throw std::runtime_error("unexpected exec observation");
        report_.exec_observed = true;
        apply(core_->exec_observed(now));
    } else if (auto *failed = std::get_if<StartFailed>(&message.message)) {
        report_.exec_error =
            "stage=" + std::to_string(failed->stage) + ", errno=" + std::to_string(failed->error);
        apply(core_->start_failed(report_.exec_error, now));
    } else if (auto *exit = std::get_if<MainExit>(&message.message)) {
        if (report_.exit_code || report_.term_signal)
            throw std::runtime_error("duplicate MAIN_EXIT");
        apply(core_->main_exited(exit->exit_code, exit->signal, now));
    } else if (std::holds_alternative<InitDone>(message.message)) {
        init_done_ = true;
    } else if (auto *error = std::get_if<InitError>(&message.message)) {
        report_.exec_error = error->message;
        apply(core_->start_failed(report_.exec_error, now));
    } else
        throw std::runtime_error("unknown init event");
}

void RunSession::supervise(SignalSource &signals) {
    EventLoop poller;
    TickSource ticks;
    auto drain_channel = [&](bool parent) {
        bool &open = parent ? launcher_channel_open_ : init_channel_open_;
        if (!open)
            return;
        auto &channel = parent ? launcher_->launcher_channel() : launcher_->init_channel();
        for (int budget = 0; budget < 32; ++budget) {
            std::optional<ControlEnvelope> message;
            try {
                message = channel.receive(true);
            } catch (const ChannelClosed &e) {
                open = false;
                channel.close();
                // A peer may close right after queuing its final facts; an
                // unexpected closure is a supervision failure, not a target result.
                if (parent && !init_reaped_) {
                    report_.errors.push_back(
                        std::string(parent ? "launcher channel: " : "init channel: ") + e.what());
                    apply(core_->supervisor_failed("launcher channel closed", now_ms()));
                }
                if (!parent && !init_done_ && core_->phase() != Phase::stopping)
                    apply(core_->supervisor_failed("init channel closed", now_ms()));
                break;
            }
            if (!message)
                break;
            if (parent)
                on_launcher(std::move(*message));
            else
                on_init(std::move(*message));
        }
    };
    for (;;) {
        auto events =
            poller.wait({{stdout_->readable_fd(), 1},
                         {stderr_->readable_fd(), 2},
                         {launcher_channel_open_ ? launcher_->launcher_channel().fd() : -1, 3},
                         {init_channel_open_ ? launcher_->init_channel().fd() : -1, 4},
                         {signals.fd(), 5},
                         {ticks.fd(), 6},
                         {control_->fd(), 7},
                         {stdout_->completion_fd(), 8},
                         {stderr_->completion_fd(), 9}});
        // Read control facts before deciding a timeout; kernel physical ordering is not inferred.
        for (auto &event : events) {
            if (event == 3)
                drain_channel(true);
            if (event == 4)
                drain_channel(false);
        }
        bool out_ready = false, err_ready = false;
        for (auto &event : events) {
            switch (event) {
            case 8:
            case 1:
                out_ready = true;
                break;
            case 9:
            case 2:
                err_ready = true;
                break;
            case 5:
                if (signals.drain_cancel())
                    request_cancel();
                break;
            case 6:
                ticks.drain();
                break;
            case 7:
                break;
            default:
                break;
            }
        }
        if (control_->accept_cancel())
            request_cancel();
        // A pipe and its writer completion may both be ready. Drain each stream
        // once per iteration so output cannot double its fairness budget.
        if (out_ready)
            stdout_->drain();
        if (err_ready)
            stderr_->drain();
        drain_channel(true);
        drain_channel(false);

        const Millis now = now_ms();
        if (launcher_->poll_reaped()) {
            if (!init_reaped_) {
                drain_channel(true);
                if (!init_reaped_)
                    apply(core_->supervisor_failed("launcher exited without confirmed init reap", now));
            }
            core_->mark_launcher_reaped();
            report_.launcher_reaped = true;
        }
        observe_streams(now);
        const bool children_gone = cgroup_ ? cgroup_->empty() : (init_done_ && init_reaped_);
        if (children_gone && !children_seen_) {
            children_seen_ = true;
            apply(core_->descendants_empty(now));
        }
        sample_metrics(); // Cancel/deadline decisions above are independent of sampling cadence.
        apply(core_->tick(now));
        if (core_->finished())
            break;
    }
}

void RunSession::finalize() {
    mark_stopping();
    transition(RunState::finalizing);
    const Millis now = now_ms();
    if (core_ && !core_->finished()) {
        report_.errors.push_back("supervision ended without a confirmed result");
        apply(core_->supervisor_failed("supervision ended without a confirmed result", now));
    }
    if (launcher_) {
        try {
            if (!launcher_->poll_reaped())
                launcher_->abort();
            report_.launcher_reaped = launcher_->poll_reaped();
            if (report_.launcher_reaped && core_)
                core_->mark_launcher_reaped();
        } catch (const std::exception &e) {
            report_.cleanup_errors.push_back(e.what());
        }
    }
    if (cgroup_) {
        try {
            if (!cgroup_->empty())
                cgroup_->kill_all();
            auto end = monotonic_ms() + 2000;
            while (!cgroup_->empty() && monotonic_ms() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            report_.descendants_empty = cgroup_->empty();
            sample_metrics(true); // Capture kernel peak/events before deleting the resource group.
            report_.metrics = sampler_->final_report();
            if (report_.descendants_empty) {
                cgroup_->remove();
                report_.cgroup_removed = true;
                if (core_)
                    core_->mark_cgroup_removed();
            } else {
                report_.cleanup_errors.push_back("cgroup still populated");
            }
        } catch (const std::exception &e) {
            report_.cleanup_errors.push_back(e.what());
        }
    }
    if (stdout_) {
        stdout_->close_incomplete();
        stdout_->settle();
        report_.out = stdout_->summary();
        if (!report_.out.write_error.empty())
            report_.errors.push_back(report_.out.write_error);
    }
    if (stderr_) {
        stderr_->close_incomplete();
        stderr_->settle();
        report_.err = stderr_->summary();
        if (!report_.err.write_error.empty())
            report_.errors.push_back(report_.err.write_error);
    }
    if (store_) {
        auto path = store_->directory() / "rootfs";
        if (::rmdir(path.c_str()) < 0 && errno != ENOENT)
            report_.cleanup_errors.push_back("rootfs directory not empty or removal failed");
        else if (core_)
            core_->mark_workspace_removed();
    }
    const auto memory_events = report_.metrics.value("memory.events.local", Json::object());
    const bool oom = memory_events.contains("oom_kill") &&
                     memory_events.at("oom_kill").is_number() &&
                     memory_events.at("oom_kill").get<std::uint64_t>() > 0;
    if (oom && core_)
        apply(core_->oom_observed(now));

    // The report is an adapter over the execution result: it never re-derives
    // lifecycle facts of its own.
    const auto &result = core_->result();
    report_.exit_code = result.outcome.exit_code();
    report_.term_signal = result.outcome.term_signal();
    report_.summary = summary_of(result.outcome);
    // OOM is discovered after the fact from cgroup counters, so by the time it is
    // known the core has usually already sealed a SIGKILL as "signaled". The old
    // report gave the memory controller precedence over an otherwise unexplained
    // kill; keep that, but never relabel a target that exited on its own.
    if (oom && result_unknown_or_signaled(result.outcome))
        report_.summary = "oom";
    report_.verdict = verdict_name(verdict_of(result));
    report_.stop_reason = stop_reason_of(result.outcome);
    // The core only fills the timing when it reaches a conclusion. On a forced
    // shutdown it does not, so fall back to wall clock rather than reporting a
    // run that took 0 ms.
    auto total = std::chrono::duration_cast<Millis>(result.timing.total);
    if (total.count() == 0) {
        const auto elapsed = static_cast<std::int64_t>(monotonic_ms()) -
                             static_cast<std::int64_t>(started_);
        total = Millis{elapsed > 0 ? elapsed : 0};
    }
    auto execution = std::chrono::duration_cast<Millis>(result.timing.execution);
    if (execution.count() == 0) {
        const auto startup = std::chrono::duration_cast<Millis>(result.timing.startup);
        const auto derived = total - startup;
        execution = derived.count() > 0 ? derived : Millis{0};
    }
    report_.startup_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<Millis>(result.timing.startup).count());
    report_.execution_ms = static_cast<std::uint64_t>(execution.count());
    report_.total_ms = static_cast<std::uint64_t>(total.count());
    report_.descendants_empty = report_.descendants_empty || result.cleanup.descendants_gone;
    report_.cgroup_removed = report_.cgroup_removed || result.cleanup.cgroup_removed;
    for (const auto &failure : result.cleanup.failures)
        report_.cleanup_errors.push_back(std::string(cleanup_stage_name(failure.stage)) + ": " +
                                        failure.detail);
    if (init_abnormal_ && report_.exec_error.empty() && result.cleanup.complete() &&
        report_.summary != "oom")
        report_.errors.push_back("init exited unexpectedly without a resource-limit explanation");
    if (store_)
        store_->report(report_);
    transition(RunState::finished);
    if (logger_) {
        logger_->info("finished: {} ({})", report_.summary, verdict_name(verdict_of(result)));
        logger_->flush();
    }
}

RunReport RunSession::run() {
    ensure_standard_fds();
    started_ = monotonic_ms();
    // Signals are blocked before any child exists; inherited ignored dispositions are reset.
    SignalSource outer_signals;
    try {
        prepare();
        supervise(outer_signals);
    } catch (const std::exception &e) {
        report_.errors.push_back(e.what());
        if (logger_)
            logger_->error("run failed: {}", e.what());
        if (core_ && !core_->finished())
            apply(core_->supervisor_failed(e.what(), now_ms()));
    }
    if (state_.current() == RunState::created) {
        if (store_)
            store_->report(report_);
        else
            throw std::runtime_error(report_.errors.empty() ? "cannot create run"
                                                            : report_.errors.front());
        return report_;
    }
    try {
        finalize();
    } catch (const std::exception &e) {
        report_.errors.push_back(std::string("finalize: ") + e.what());
        if (store_) {
            try {
                store_->report(report_);
            } catch (...) {
            }
        }
    }
    stdout_.reset();
    stderr_.reset();
    return report_;
}
} // namespace sandbox
