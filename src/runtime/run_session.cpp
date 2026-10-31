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
namespace sandbox {
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
    // Persistence errors remain host errors, rather than being hidden as collector failures.
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
void RunSession::prepare() {
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
    control_ = std::make_unique<ControlServer>(store_->directory() / "control.sock");
    if (config_.mode == "isolated")
        cgroup_ = std::make_unique<CgroupV2>(config_.cgroup_root, store_->id(), config_.limits);
    if (cgroup_)
        sampler_ = std::make_unique<MetricsSampler>(cgroup_->path(), MetricRegistry::defaults(),
                                                    config_.metrics.interval_ms,
                                                    config_.metrics.history_limit);
    auto root = store_->directory() / "rootfs";
    checked(::mkdir(root.c_str(), 0700), "create root mountpoint");
    auto out = Pipe::create(), err = Pipe::create();
    auto out_file = open_path(store_->directory() / "stdout.bin", O_WRONLY | O_CREAT | O_EXCL);
    auto err_file = open_path(store_->directory() / "stderr.bin", O_WRONLY | O_CREAT | O_EXCL);
    auto in = open_path(config_.stdin_file.empty() ? fs::path("/dev/null") : config_.stdin_file,
                        O_RDONLY);
    stdout_ = std::make_unique<OutputCapture>(std::move(out.read), std::move(out_file),
                                              config_.limits.capture_bytes);
    stderr_ = std::make_unique<OutputCapture>(std::move(err.read), std::move(err_file),
                                              config_.limits.capture_bytes);
    transition(RunState::starting);
    launcher_ = std::make_unique<LauncherClient>(config_, launcher_path_, root, in.get(),
                                                 out.write.get(), err.write.get());
    // out/err write ends are closed on return, leaving only the target environment's copies.
}
void RunSession::on_launcher(Message message) {
    auto kind = message.body.at("kind").get<std::string>();
    auto p = message.body.at("payload");
    if (kind == "INIT_CREATED") {
        if (init_created_ || (state_.current() != RunState::starting && !stopping_) || !message.fd)
            throw std::runtime_error("unexpected init identity");
        auto pid = p.at("pid").get<pid_t>();
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
        launcher_->init_channel().send(stopping_ ? "STOP" : "PREPARE");
        logger_->info("init created and resources attached, pid={}", pid);
    } else if (kind == "INIT_REAPED") {
        if (!init_created_ || init_reaped_)
            throw std::runtime_error("unexpected init reap");
        init_reaped_ = true;
        int status = p.at("wait_status").get<int>();
        init_abnormal_ = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    } else if (kind == "BOOTSTRAP_ERROR")
        throw std::runtime_error("launcher: " + p.at("error").get<std::string>());
    else
        throw std::runtime_error("unknown launcher event: " + kind);
}
void RunSession::on_init(Message message) {
    auto kind = message.body.at("kind").get<std::string>();
    auto p = message.body.at("payload");
    if (kind == "ENV_READY") {
        if (!init_created_ || env_ready_)
            throw std::runtime_error("unexpected ENV_READY");
        env_ready_ = true;
        report_.runtime_prepared = true;
        if (stopping_)
            return; // STOP is queued; do not send EXEC after cancellation.
        exec_at_ = monotonic_ms();
        report_.startup_ms = exec_at_ - started_;
        launcher_->init_channel().send("EXEC");
        transition(RunState::running);
    } else if (kind == "EXEC_OBSERVED") {
        if (!env_ready_ || report_.exec_observed)
            throw std::runtime_error("unexpected exec observation");
        report_.exec_observed = true;
    } else if (kind == "START_FAILED") {
        report_.exec_error = "stage=" + std::to_string(p.at("stage").get<int>()) +
                             ", errno=" + std::to_string(p.at("errno").get<int>());
        request_stop("startup_failure");
    } else if (kind == "MAIN_EXIT") {
        if (report_.exit_code || report_.term_signal)
            throw std::runtime_error("duplicate MAIN_EXIT");
        if (!p.at("exit_code").is_null())
            report_.exit_code = p.at("exit_code").get<int>();
        if (!p.at("signal").is_null())
            report_.term_signal = p.at("signal").get<int>();
        request_stop("main_exited");
    } else if (kind == "INIT_DONE") {
        init_done_ = true;
    } else if (kind == "INIT_ERROR") {
        report_.exec_error = p.at("error").get<std::string>();
        request_stop("startup_failure");
    } else
        throw std::runtime_error("unknown init event: " + kind);
}
void RunSession::request_stop(const std::string &reason) {
    if (stopping_)
        return;
    stopping_ = true;
    stop_at_ = monotonic_ms();
    report_.stop_reason = reason;
    transition(RunState::stopping);
    if (logger_)
        logger_->warn("stop requested: {}", reason);
    if (launcher_ && init_created_ && init_channel_open_) {
        try {
            launcher_->init_channel().send("STOP");
        } catch (const std::exception &e) {
            if (logger_)
                logger_->warn("stop channel: {}", e.what());
        }
    }
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
            std::optional<Message> message;
            try {
                message = channel.receive(true);
            } catch (const ChannelClosed &e) {
                open = false;
                channel.close();
                // Peer can close after queuing its final facts. Unexpected closure is retained as
                // an error.
                if (parent && !init_reaped_) {
                    report_.errors.push_back(
                        std::string(parent ? "launcher channel: " : "init channel: ") + e.what());
                    request_stop("host_error");
                }
                if (!parent && !init_done_ && !stopping_)
                    request_stop("init_channel_closed");
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
                    request_stop("cancelled");
                break;
            case 6:
                ticks.drain();
                break;
            case 7:
                if (control_->accept_cancel())
                    request_stop("cancelled");
                break;
            default:
                break;
            }
        }
        if (control_->accept_cancel())
            request_stop("cancelled");
        // A pipe and its writer completion may both be ready. Drain each stream
        // once per iteration so output cannot double its fairness budget.
        if (out_ready) stdout_->drain();
        if (err_ready) stderr_->drain();
        // waitpid/EOF and peer messages are separate observations. Drain both channels even
        // if readiness arrived after this epoll batch, before deciding the run is complete.
        drain_channel(true);
        drain_channel(false);
        auto now = monotonic_ms();
        if (!stopping_ && !env_ready_ && now - started_ >= config_.limits.startup_ms)
            request_stop("startup_timeout");
        if (!stopping_ && exec_at_ && now - exec_at_ >= config_.limits.wall_ms)
            request_stop("timeout");
        report_.launcher_reaped = launcher_->poll_reaped();
        if (report_.launcher_reaped && !init_reaped_) {
            drain_channel(true);
            if (!init_reaped_) {
                report_.errors.push_back("launcher exited without confirmed init reap");
                request_stop("host_error");
            }
        }
        const auto force_after = config_.limits.grace_ms + (cgroup_ ? 0 : 1000);
        if (stopping_ && now - stop_at_ >= force_after && !forced_) {
            forced_ = true;
            if (cgroup_)
                cgroup_->kill_all();
            else
                send_pidfd_signal(launcher_->init_pidfd(), SIGKILL);
        }
        sample_metrics(); // Cancel/deadline decisions above are independent of sampling cadence.
        if (cgroup_)
            report_.descendants_empty = cgroup_->empty();
        else
            report_.descendants_empty = init_done_ && init_reaped_;
        if (report_.launcher_reaped && init_reaped_ && (init_done_ || !init_channel_open_) &&
            report_.descendants_empty && stdout_->complete() && stderr_->complete())
            break;
        if (stopping_ && now - stop_at_ > config_.limits.grace_ms + 4000) {
            report_.cleanup_errors.push_back("supervision completion deadline exceeded");
            if (!report_.descendants_empty)
                report_.cleanup_errors.push_back("descendants not confirmed empty before deadline");
            if (!stdout_->complete() || !stderr_->complete())
                report_.cleanup_errors.push_back("output EOF/writer completion deadline exceeded");
            break;
        }
    }
}
void RunSession::finalize() {
    if (!stopping_)
        request_stop("host_error");
    transition(RunState::finalizing);
    if (launcher_) {
        try {
            if (!launcher_->poll_reaped())
                launcher_->abort();
            report_.launcher_reaped = launcher_->poll_reaped();
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
            } else
                report_.cleanup_errors.push_back("cgroup still populated");
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
    }
    auto now = monotonic_ms();
    report_.total_ms = now - started_;
    if (exec_at_)
        report_.execution_ms = now - exec_at_;
    if (report_.stop_reason == "main_exited")
        report_.stop_reason.clear();
    const auto memory_events = report_.metrics.value("memory.events.local", Json::object());
    const bool oom = memory_events.contains("oom_kill") &&
                     memory_events.at("oom_kill").is_number() &&
                     memory_events.at("oom_kill").get<std::uint64_t>() > 0;
    if (oom && report_.stop_reason != "timeout" && report_.stop_reason != "cancelled")
        report_.stop_reason = "oom";
    else if (init_abnormal_ && report_.exec_error.empty() &&
             (report_.stop_reason.empty() || report_.stop_reason == "init_channel_closed"))
        report_.errors.push_back("init exited unexpectedly without a resource-limit explanation");
    report_.conclude();
    if (store_)
        store_->report(report_);
    transition(RunState::finished);
    if (logger_) {
        logger_->info("finished: {}", report_.summary);
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
        report_.conclude();
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
