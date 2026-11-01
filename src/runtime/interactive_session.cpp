#include "sandbox/command_execution.hpp"
#include "sandbox/core/ops_json.hpp"
#include "sandbox/event_loop.hpp"
#include "sandbox/interactive.hpp"
#include "sandbox/session.hpp"
#include "sandbox/session_policy.hpp"
#include "sandbox/session_safety.hpp"
#include "sandbox/session_state.hpp"
#include "sandbox/session_store.hpp"
#include "sandbox/snapshot.hpp"
#include "sandbox/workspace_service.hpp"
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
namespace sandbox {
namespace {
StopReason stop_reason_of(CommandStop stop) {
    switch (stop) {
    case CommandStop::main_exit:
        return StopReason::exited;
    case CommandStop::cancelled:
        return StopReason::cancelled;
    case CommandStop::timeout:
        return StopReason::timed_out;
    case CommandStop::session_closed:
        return StopReason::session_closed;
    case CommandStop::startup_failure:
        return StopReason::startup_failed;
    case CommandStop::supervision_failure:
        return StopReason::supervision_failed;
    }
    return StopReason::supervision_failed;
}
// The session still accepts the legacy request type on its wire entry point; this
// converts it once, so everything below works on the typed request.
ExecutionRequest intent_of(const ExecuteRequest &request) {
    std::optional<RequestKey> key;
    if (request.request_id) {
        key = RequestKey::parse(request.request_id->str());
        if (!key)
            throw SessionError("INVALID_REQUEST", "invalid request_id");
    }
    return ExecutionRequest::create(request.argv, request.cwd, request.env, request.timeout,
                                    std::move(key));
}
OutputResult output_of(const StreamReport &report) {
    OutputResult output;
    output.saved_bytes = report.saved;
    output.discarded_bytes = report.discarded;
    output.failed_bytes = report.failed_bytes;
    output.queue_peak_bytes = report.queue_peak_bytes;
    output.backpressure_events = report.backpressure_events;
    output.eof = report.eof;
    output.flushed = report.flushed;
    output.write_error = report.write_error;
    return output;
}
ExecutionOutcome outcome_of(const CommandFacts &facts) {
    switch (facts.reason) {
    case StopReason::exited:
        if (facts.signal && *facts.signal != no_signal)
            return ExecutionOutcome(Signaled{*facts.signal});
        return ExecutionOutcome(Exited{facts.exit_code.value_or(no_exit_code)});
    case StopReason::cancelled:
        return ExecutionOutcome(Cancelled{CancelCause::requested});
    case StopReason::session_closed:
        return ExecutionOutcome(Cancelled{CancelCause::session_closed});
    case StopReason::timed_out:
        return ExecutionOutcome(TimedOut{});
    case StopReason::startup_failed:
        return ExecutionOutcome(
            StartupFailed{"exec failed", facts.exec_errno.value_or(0)});
    case StopReason::supervision_failed:
        return ExecutionOutcome(SupervisionFailed{"command supervision failed"});
    }
    return ExecutionOutcome(SupervisionFailed{"unknown command stop reason"});
}
// Durable completion for one session command. The command-level cleanup facts are
// the ones init established before reporting: it only reports a command done after
// observing the descendant set empty.
FinishedCommand finished_of(const CommandRecord &record) {
    if (!record.facts)
        throw std::logic_error("command finished without terminal facts");
    const auto &facts = *record.facts;
    FinishedCommand finished{*CommandIndex::parse(record.id.str()), ExecutionResult{},
                             std::chrono::system_clock::now()};
    finished.result.outcome = outcome_of(facts);
    finished.result.out = output_of(record.out);
    finished.result.err = output_of(record.err);
    finished.result.cleanup.descendants_gone = true;
    finished.result.timing.execution = facts.elapsed;
    return finished;
}
} // namespace
namespace {
bool overlap(const fs::path &a, const fs::path &b) {
    auto x = a.begin(), y = b.begin();
    while (x != a.end() && y != b.end() && *x == *y) {
        ++x;
        ++y;
    }
    return x == a.end() || y == b.end();
}
class SessionController {
    RunConfig config_;
    fs::path root_, launcher_path_;
    std::unique_ptr<RunStore> store_;
    std::unique_ptr<ControlServer> server_;
    std::unique_ptr<CgroupV2> group_;
    std::unique_ptr<MetricsSampler> sampler_;
    std::unique_ptr<LauncherClient> launcher_;
    std::unique_ptr<CommandExecution> execution_;
    std::unique_ptr<SessionStore> records_;
    std::unique_ptr<WorkspaceService> workspace_;
    // Snapshot copies this session made of /input; removed with the workspace.
    std::vector<fs::path> input_snapshots_;
    SessionLifecycle lifecycle_;
    Json last_ = nullptr;
    Json current() const {
        return execution_ && execution_->record ? execution_->record->json() : Json(nullptr);
    }
    bool done_ = false, reaped_ = false, parent_open_ = true, init_open_ = true;
    std::uint64_t created_ = 0, idle_ = 0, closed_ = 0;
    unsigned counter_ = 0;
    std::string reason_;
    Json errors_ = Json::array();
    std::string metric_error_;
    void sample_metrics(std::uint64_t now, bool force = false) {
        if (!sampler_)
            return;
        try {
            sampler_->sample(now, force);
            metric_error_.clear();
        } catch (const std::exception &e) {
            metric_error_ = e.what();
        }
    }
    Json metrics_report(bool live = false) const {
        if (!sampler_)
            return {{"available", false}, {"reason", "no per-session cgroup metrics"}};
        if (!metric_error_.empty())
            return {{"available", false}, {"reason", metric_error_}};
        try {
            auto result = live ? sampler_->live_snapshot() : sampler_->final_report();
            result["available"] = true;
            return result;
        } catch (const std::exception &e) {
            return {{"available", false}, {"reason", e.what()}};
        }
    }
    Json snapshot() const {
        auto summary = [](Json value) {
            if (value.is_object()) {
                value.erase("argv");
                value.erase("cwd");
            }
            return value;
        };
        return {{"run_id", store_->id()},
                {"schema_version", 2},
                {"protocol_version", session_protocol_version},
                {"session_state", session_state_name(lifecycle_.state())},
                {"current", summary(current())},
                {"last", summary(last_)},
                {"commands", counter_},
                {"metrics", metrics_report(true)}};
    }
    void persist() {
        records_->snapshot(snapshot());
    }
    void close(const std::string &reason) {
        if (lifecycle_.stopping())
            return;
        lifecycle_.transition(SessionState::closing);
        closed_ = monotonic_ms();
        reason_ = reason;
        if (execution_)
            execution_->input.write.reset();
        if (launcher_ && init_open_)
            launcher_->init_channel().send(Stop{});
        persist();
    }
    Json start(const ExecuteRequest &request) {
        const auto intent = intent_of(request);
        if (auto existing = records_->replay(intent, current()))
            return *existing;
        lifecycle_.require_idle();
        if (counter_ >= 1024)
            throw SessionError("COMMAND_LIMIT", "session command count limit 1024 reached");
        RunConfig command;
        try {
            command = resolve_command(config_, request);
        } catch (const SessionError &) {
            throw;
        } catch (const std::exception &e) {
            throw SessionError("INVALID_REQUEST", e.what());
        }
        auto id = "cmd-" + std::to_string(++counter_);
        const auto index = *CommandIndex::parse(id);
        auto dir = store_->directory() / id;
        checked(::mkdir(dir.c_str(), 0700), "create command directory");
        auto prepared = std::make_unique<CommandExecution>(dir, config_.limits.capture_bytes);
        auto started = monotonic_ms();
        sample_metrics(started, true);
        auto baseline = metrics_report();
        prepared->record.emplace(CommandId::parse(id), request, command);
        const std::string dispatch = command.to_json().dump();
        prepared->baseline = std::move(baseline);
        prepared->started = started;
        prepared->wall_ms = command.limits.wall_ms;
        // Durable acceptance is the boundary after which this request cannot be rerun.
        records_->accept(AcceptedCommand{index, intent, std::chrono::system_clock::now(),
                                         fs::path(command.cwd)});
        execution_ = std::move(prepared);
        lifecycle_.transition(SessionState::running);
        auto &channel = launcher_->init_channel();
        try {
            channel.send(StdinHandle{index}, execution_->input.read.get());
            channel.send(StdoutHandle{index}, execution_->output.write.get());
            channel.send(StderrHandle{index}, execution_->error.write.get());
            channel.send(Command{index, dispatch});
            execution_->release_child_ends();
            persist();
        } catch (...) {
            execution_->release_child_ends();
            close("command_dispatch_failed");
            throw;
        }
        return current();
    }
    Json write_stdin(const WriteStdinRequest &input) {
        if (!execution_ || !execution_->record || execution_->record->id != input.command_id)
            throw SessionError("COMMAND_NOT_ACTIVE", "command is no longer active");
        lifecycle_.require_running();
        if (!execution_->input.write)
            throw std::runtime_error("stdin is closed");
        if (input.bytes.size() > 4096)
            throw std::runtime_error("stdin chunk exceeds 4096 bytes");
        auto n = ::write(execution_->input.write.get(), input.bytes.data(), input.bytes.size());
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) n = 0;
        if (n < 0) system_error("write command stdin");
        return {{"accepted_bytes", n}};
    }
    // Typed dispatch. The wire shape is decoded once, in the overload below, so
    // every branch here works on validated business types.
    Json handle(const SessionRequest &request) {
        const auto check_active = [&](const CommandIndex &command) {
            if (!execution_ || !execution_->record ||
                execution_->record->id.str() != command.str())
                throw SessionError("COMMAND_NOT_ACTIVE", "command is no longer active");
        };
        return std::visit(
            [&](const auto &value) -> Json {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Status>) {
                    return snapshot();
                } else if constexpr (std::is_same_v<T, RequestStatusQuery>) {
                    return records_->request_status(value.key, current());
                } else if constexpr (std::is_same_v<T, Execute>) {
                    // Transitional: the storage layer still speaks the legacy
                    // request type, so the typed request is converted once here.
                    const auto &typed = value.request;
                    std::optional<std::string> key;
                    if (typed.key())
                        key = typed.key()->str();
                    std::optional<std::string> cwd;
                    if (typed.cwd())
                        cwd = typed.cwd()->string();
                    std::optional<std::uint64_t> wall;
                    if (typed.timeout())
                        wall = static_cast<std::uint64_t>(typed.timeout()->count());
                    return start(ExecuteRequest::from_fields(std::move(key), typed.argv(),
                                                            std::move(cwd), typed.env(), wall));
                } else if constexpr (std::is_same_v<T, Close>) {
                    close("closed");
                    return {{"status", "accepted"}};
                } else if constexpr (std::is_same_v<T, Cancel>) {
                    // A named command must be the active one whether or not there
                    // is anything to cancel: an old handle may not address a newer
                    // command, and must not silently succeed either.
                    if (value.command)
                        check_active(*value.command);
                    if (!current().is_null() && !lifecycle_.stopping()) {
                        lifecycle_.require_running();
                        launcher_->init_channel().send(CancelCommand{});
                        if (execution_)
                            execution_->input.write.reset();
                    }
                    return {{"status", "accepted"}, {"active", !current().is_null()}};
                } else if constexpr (std::is_same_v<T, StdinWrite>) {
                    check_active(value.command);
                    return write_stdin({CommandId::parse(value.command.str()), value.bytes});
                } else if constexpr (std::is_same_v<T, StdinClose>) {
                    check_active(value.command);
                    lifecycle_.require_running();
                    if (execution_)
                        execution_->input.write.reset();
                    return {{"closed", true}};
                } else if constexpr (std::is_same_v<T, ReadResult>) {
                    return records_->result(value.command);
                } else if constexpr (std::is_same_v<T, ReadOutput>) {
                    if (value.offset > config_.limits.capture_bytes)
                        throw std::runtime_error("offset exceeds capture limit");
                    const auto path = store_->directory() / value.command.str();
                    const auto stream =
                        value.stream == Stream::standard_output ? "stdout" : "stderr";
                    auto fd = open_path(path / (std::string(stream) + ".bin"), O_RDONLY);
                    unsigned char data[2048];
                    auto n = ::pread(fd.get(), data, sizeof(data), static_cast<off_t>(value.offset));
                    checked(static_cast<int>(n), "read captured output");
                    return {{"offset", value.offset},
                            {"next_offset", value.offset + static_cast<std::uint64_t>(n)},
                            {"bytes", std::vector<unsigned char>(data, data + n)},
                            {"finished", fs::exists(path / "result.json")}};
                } else {
                    lifecycle_.require_idle();
                    if (config_.mode != "isolated")
                        throw SessionError("POLICY_DENIED", "managed files require isolated mode");
                    return workspace_->handle(value);
                }
            },
            request.operation());
    }
    void event(ControlEnvelope envelope, bool parent) {
        if (parent) {
            if (auto *created = std::get_if<InitCreated>(&envelope.message)) {
                auto pid = created->pid;
                launcher_->set_init_identity(pid, std::move(envelope.fd));
                store_->owner_update({{"init_pid", pid}, {"init_start", process_start(pid)}});
                if (group_) {
                    auto proc = fs::path("/proc") / std::to_string(pid);
                    write_control_file(proc / "setgroups", "deny");
                    write_control_file(proc / "uid_map", "0 " + std::to_string(::getuid()) + " 1");
                    write_control_file(proc / "gid_map", "0 " + std::to_string(::getgid()) + " 1");
                    group_->attach(pid);
                    sample_metrics(monotonic_ms(), true);
                }
                launcher_->init_channel().send(lifecycle_.stopping() ? ControlMessage{Stop{}}
                                                                  : ControlMessage{Prepare{}});
            } else if (auto *reaped = std::get_if<InitReaped>(&envelope.message)) {
                reaped_ = true;
                int status = reaped->wait_status;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    throw std::runtime_error("session init exited abnormally");
            } else
                throw std::runtime_error("launcher failed: unexpected message");
        } else {
            if (std::holds_alternative<EnvReady>(envelope.message)) {
                if (!lifecycle_.stopping())
                    lifecycle_.transition(SessionState::idle);
                idle_ = monotonic_ms();
                store_->state(RunState::running);
                persist();
            } else if (auto *done = std::get_if<CommandDone>(&envelope.message)) {
                if (!execution_ || !execution_->record)
                    throw std::runtime_error("command result mismatch");
                auto facts = CommandFacts{CommandId::parse(done->command.str()), done->exit_code,
                                          done->signal, done->exec_errno,
                                          stop_reason_of(done->stop), done->elapsed};
                if (facts.id != execution_->record->id)
                    throw std::runtime_error("command result mismatch");
                execution_->record->apply(std::move(facts));
                if (!lifecycle_.stopping())
                    lifecycle_.transition(SessionState::draining);
                execution_->done = true;
                if (execution_)
                    execution_->input.write.reset();
            } else if (std::holds_alternative<InitDone>(envelope.message))
                done_ = true;
            else
                throw std::runtime_error("init failed: unexpected message");
        }
    }

    void drain_channels() {
        for (bool parent : {true, false}) {
            bool &open = parent ? parent_open_ : init_open_;
            if (!open)
                continue;
            auto &channel = parent ? launcher_->launcher_channel() : launcher_->init_channel();
            for (int budget = 0; budget < 32; ++budget) {
                try {
                    auto message = channel.receive(true);
                    if (!message)
                        break;
                    event(std::move(*message), parent);
                } catch (const ChannelClosed &) {
                    open = false;
                    channel.close();
                    if (!(parent ? reaped_ : done_))
                        throw;
                    break;
                }
            }
        }
    }

  public:
    SessionController(RunConfig c, fs::path root, fs::path launcher)
        : config_(std::move(c)), root_(std::move(root)), launcher_path_(std::move(launcher)) {}
    Json run() {
        ensure_standard_fds();
        SignalSource signals;
        EventLoop loop;
        TickSource ticks;
        struct PipeSignalGuard {
            using Handler = void (*)(int);
            Handler old = ::signal(SIGPIPE, SIG_IGN);
            ~PipeSignalGuard() {
                ::signal(SIGPIPE, old);
            }
        } pipe_signal_guard;
        created_ = idle_ = monotonic_ms();
        auto state = fs::weakly_canonical(fs::absolute(root_));
        for (auto &m : config_.mounts)
            if (overlap(state, fs::canonical(m.source)))
                throw std::runtime_error("state/mount overlap");
        if (!config_.runtime_root.empty() && overlap(state, fs::canonical(config_.runtime_root)))
            throw std::runtime_error("state/runtime overlap");
        if (config_.mode == "isolated")
            for (auto &m : config_.mounts)
                if (m.target == "/output")
                    throw std::runtime_error("session owns /output; remove external output mount");
        store_ = RunStore::create(root_, config_);
        store_->owner_update({{"persistent", true}});
        records_ = std::make_unique<SessionStore>(*store_);
        workspace_ =
            std::make_unique<WorkspaceService>(store_->directory() / "workspace", *records_,
                                               config_.workspace);
        try {
            auto rootfs = store_->directory() / "rootfs";
            checked(::mkdir(rootfs.c_str(), 0700), "create session rootfs");
            auto workspace = store_->directory() / "workspace";
            checked(::mkdir(workspace.c_str(), 0700), "create workspace");
            if (config_.mode == "isolated")
                config_.mounts.push_back({workspace, "/output", false});
            // Snapshot /input before the launcher exists and before the effective
            // config is published, so config.json describes what actually ran.
            input_snapshots_ = snapshot_inputs(config_, store_->directory());
            store_->write_json("config.json", config_.to_json());
            store_->state(RunState::preparing);
            server_ = std::make_unique<ControlServer>(store_->directory() / "control.sock");
            persist();
            if (config_.mode == "isolated") {
                group_ =
                    std::make_unique<CgroupV2>(config_.cgroup_root, store_->id(), config_.limits);
                sampler_ = std::make_unique<MetricsSampler>(
                    group_->path(), MetricRegistry::defaults(), config_.metrics.interval_ms,
                    config_.metrics.history_limit);
            }
            auto null = open_path("/dev/null", O_RDWR);
            launcher_ = std::make_unique<LauncherClient>(config_, launcher_path_, rootfs,
                                                         null.get(), null.get(), null.get());
            while (true) {
                if (execution_)
                    execution_->drain();
                drain_channels();
                if (execution_ && execution_->drained()) {
                    Json metrics = {{"available", static_cast<bool>(sampler_)},
                                    {"scope", "session_cgroup_during_command"},
                                    {"max_sampled_memory_bytes", execution_->memory_peak}};
                    if (sampler_) {
                        sample_metrics(monotonic_ms(), true);
                        auto raw = metrics_report();
                        metrics["available"] = raw.value("available", false);
                        if (raw.contains("reason"))
                            metrics["reason"] = raw.at("reason");
                        for (auto key : {"usage_usec", "user_usec", "system_usec"}) {
                            metrics["cpu.stat"][key] =
                                session_counter_delta(execution_->baseline, raw, key);
                            if (metrics["cpu.stat"][key].is_null())
                                metrics["unavailable"][key] =
                                    "missing, invalid or regressed counter";
                        }
                    }
                    execution_->finish(std::move(metrics));
                    records_->finish(finished_of(*execution_->record));
                    last_ = current();
                    execution_.reset();
                    if (!lifecycle_.stopping())
                        lifecycle_.transition(SessionState::idle);
                    idle_ = monotonic_ms();
                    persist();
                }
                auto now = monotonic_ms();
                if (signals.drain_cancel())
                    close("signal");
                if (!lifecycle_.stopping() && now - created_ >= config_.session_ms)
                    close("session_timeout");
                if (!lifecycle_.stopping() && lifecycle_.state() == SessionState::preparing &&
                    now - created_ >= config_.limits.startup_ms)
                    close("startup_timeout");
                if (!lifecycle_.stopping() && lifecycle_.state() == SessionState::idle &&
                    current().is_null() && now - idle_ >= config_.idle_ms)
                    close("idle_timeout");
                if (!lifecycle_.stopping() && !current().is_null() &&
                    now - execution_->started >
                        execution_->wall_ms + config_.limits.grace_ms + 5000)
                    close("command_supervision_timeout");
                if (sampler_) {
                    sample_metrics(now);
                    auto latest = metrics_report(true).value("latest", Json(nullptr));
                    if (!current().is_null() && latest.is_object()) {
                        auto memory = latest.value("memory_current_bytes", Json(nullptr));
                        if (memory.is_number_unsigned() &&
                            (execution_->memory_peak.is_null() ||
                             memory.get<std::uint64_t>() >
                                 execution_->memory_peak.get<std::uint64_t>()))
                            execution_->memory_peak = memory;
                    }
                }
                bool exited = launcher_->poll_reaped();
                if (exited)
                    drain_channels(); // waitpid and queued protocol facts are independent.
                if (exited && reaped_ && done_ && current().is_null())
                    break;
                if (exited && (!reaped_ || !done_))
                    throw std::runtime_error("session process exited without complete facts");
                if (lifecycle_.stopping() && now - closed_ > config_.limits.grace_ms + 5000)
                    throw std::runtime_error("session close deadline exceeded");
                server_->serve([&](const SessionRequest &request) -> Json {
                    try {
                        return handle(request);
                    } catch (const StorageError &e) {
                        errors_.push_back(e.what());
                        try {
                            close("storage_failure");
                        } catch (...) {
                        }
                        throw;
                    }
                });
                loop.wait({{signals.fd(), 1},
                           {ticks.fd(), 2},
                           {server_->fd(), 3},
                           {parent_open_ ? launcher_->launcher_channel().fd() : -1, 4},
                           {init_open_ ? launcher_->init_channel().fd() : -1, 5},
                           {execution_ ? execution_->out->readable_fd() : -1, 6},
                           {execution_ ? execution_->err->readable_fd() : -1, 7},
                           {execution_ ? execution_->out->completion_fd() : -1, 8},
                           {execution_ ? execution_->err->completion_fd() : -1, 9}});
                ticks.drain();
            }
        } catch (const std::exception &e) {
            errors_.push_back(e.what());
            reason_ = "host_error";
            if (lifecycle_.state() != SessionState::failed)
                lifecycle_.transition(SessionState::failed);
        }
        if (execution_)
            execution_->input.write.reset();
        if (group_)
            try {
                group_->kill_all();
            } catch (const std::exception &e) {
                errors_.push_back(e.what());
            }
        if (launcher_)
            try {
                launcher_->abort();
            } catch (const std::exception &e) {
                errors_.push_back(e.what());
            }
        if (execution_) {
            execution_->out->close_incomplete();
            execution_->err->close_incomplete();
            execution_->out->settle();
            execution_->err->settle();
        }
        if (sampler_)
            try {
                sample_metrics(monotonic_ms(), true);
            } catch (const std::exception &e) {
                errors_.push_back(e.what());
            }
        if (group_)
            try {
                auto until = monotonic_ms() + 2000;
                while (!group_->empty() && monotonic_ms() < until) {
                    pollfd wait{-1, 0, 0};
                    ::poll(&wait, 1, 20);
                }
                group_->remove();
            } catch (const std::exception &e) {
                errors_.push_back(e.what());
            }
        ::rmdir((store_->directory() / "rootfs").c_str());
        if (!lifecycle_.stopping())
            lifecycle_.transition(SessionState::closing);
        lifecycle_.transition(SessionState::closed);
        auto result = records_->recover(snapshot());
        result["session_state"] = "closed";
        result["stop_reason"] = reason_;
        result["errors"] = errors_;
        result["metrics"] = metrics_report();
        // Retention is policy, and a failed removal is reported rather than
        // hidden: silently keeping a workspace the caller asked to drop is the
        // same class of mistake as silently deleting one they wanted.
        if (!config_.workspace.keep_on_close) {
            // Appended to whatever cleanup already failed: assigning here would
            // silently replace earlier failures.
            auto failures = result.value("cleanup_errors", Json::array());
            for (const auto &directory : input_snapshots_) {
                std::error_code failure;
                fs::remove_all(directory, failure);
                if (failure)
                    failures.push_back("input snapshot: " + failure.message());
            }
            std::error_code failure;
            fs::remove_all(store_->directory() / "workspace", failure);
            if (failure)
                failures.push_back(failure.message());
            if (!failures.empty())
                result["cleanup_errors"] = failures;
        }
        records_->close(result);
        execution_.reset();
        server_.reset();
        return result;
    }
};
} // namespace
Json serve_session(RunConfig config, const fs::path &state, const fs::path &launcher) {
    config.persistent = true;
    return SessionController(std::move(config), state, launcher).run();
}
} // namespace sandbox
