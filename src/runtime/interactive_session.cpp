#include "sandbox/command_execution.hpp"
#include "sandbox/core/ops_json.hpp"
#include "sandbox/event_loop.hpp"
#include "sandbox/interactive.hpp"
#include "sandbox/isolation.hpp"
#include "sandbox/session.hpp"
#include "sandbox/session_policy.hpp"
#include "sandbox/session_safety.hpp"
#include "sandbox/session_state.hpp"
#include "sandbox/session_store.hpp"
#include "sandbox/session_wire.hpp"
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
// The command's own budget. Grace is deliberately the same backstop the session
// already used (wall + grace + 5000): for a session, killing the group is not a
// per-command operation, so the core must not escalate before init has had its
// own chance to take the command down.
SupervisionLimits command_limits(const RunConfig &command) {
    SupervisionLimits limits;
    limits.startup = Millis{static_cast<std::int64_t>(command.limits.startup_ms)};
    limits.wall = Millis{static_cast<std::int64_t>(command.limits.wall_ms)};
    limits.grace = Millis{static_cast<std::int64_t>(command.limits.grace_ms) + 5000};
    limits.drain = Millis{5000};
    limits.settle = Millis{4000};
    return limits;
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
    // The live summary of the command in flight. Once it is finished the durable
    // record is the answer instead, so this never claims a terminal state.
    Json current() const {
        if (!execution_ || !execution_->accepted)
            return nullptr;
        const auto &accepted = *execution_->accepted;
        Json value = {{"command_id", accepted.index.str()},
                      {"argv", accepted.request.argv()},
                      {"state", "running"}};
        if (accepted.effective_cwd)
            value["cwd"] = accepted.effective_cwd->string();
        if (accepted.request.key())
            value["request_id"] = accepted.request.key()->str();
        return value;
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
    // A report about the command running right now is answered from the live
    // summary: storage only knows what was written, and a replay must not claim
    // "uncertain" about a command the caller can still talk to.
    Json command_reply(const CommandReport &report) const {
        const auto live = current();
        if (!report.finished && live.is_object() &&
            live.value("command_id", "") == report.accepted.index.str())
            return live;
        return session_wire::command_report_to_json(report);
    }
    // Apply the core's decisions. The action vocabulary is shared with single
    // runs; what differs is who owns the process group:
    //   SignalTarget -> init owns it, so "stop the target" means "cancel this command"
    //   KillTarget   -> a session cannot signal one command's group on its own, so
    //                   the escalation that is actually available is closing the
    //                   session and letting init take everything down
    //   ForceDrain   -> stop accepting new output so the writers can finish
    void observe_core(const std::vector<SupervisionAction> &actions) {
        for (const auto &action : actions) {
            std::visit(
                [&](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, SignalTarget>) {
                        // The command's group is already reported gone; a cancel
                        // now would address the next command instead.
                        if (!execution_ || execution_->done || !launcher_)
                            return;
                        try {
                            launcher_->init_channel().send(CancelCommand{});
                        } catch (const std::exception &e) {
                            errors_.push_back(std::string("cancel: ") + e.what());
                        }
                    } else if constexpr (std::is_same_v<T, KillTarget>) {
                        if (!lifecycle_.stopping())
                            close("command_supervision_timeout");
                    } else if constexpr (std::is_same_v<T, ForceDrain>) {
                        if (execution_) {
                            execution_->out->close_incomplete();
                            execution_->err->close_incomplete();
                        }
                    }
                },
                action);
        }
    }
    // Streaming facts for the core: EOF and "the writer finished" are different
    // facts, so each stream reports both exactly once.
    void observe_outputs(std::uint64_t now) {
        if (!execution_ || !execution_->core)
            return;
        auto &command = *execution_;
        const Millis when{static_cast<std::int64_t>(now)};
        const auto out = command.out->summary();
        if (!command.out_eof_seen && out.eof) {
            command.out_eof_seen = true;
            observe_core(command.core->output_eof(false, when));
        }
        if (!command.out_done_seen && command.out->complete()) {
            command.out_done_seen = true;
            observe_core(command.core->output_finished(false, stats_of(out), when));
        }
        const auto err = command.err->summary();
        if (!command.err_eof_seen && err.eof) {
            command.err_eof_seen = true;
            observe_core(command.core->output_eof(true, when));
        }
        if (!command.err_done_seen && command.err->complete()) {
            command.err_done_seen = true;
            observe_core(command.core->output_finished(true, stats_of(err), when));
        }
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
        if (auto existing = records_->replay(intent))
            return command_reply(*existing);
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
        const std::string dispatch = command.to_json().dump();
        prepared->started = started;
        prepared->wall_ms = command.limits.wall_ms;
        prepared->accepted = AcceptedCommand{
            index, intent, std::chrono::system_clock::now(), fs::path(command.cwd)};
        // The same execution core a single run uses. The environment is already
        // prepared, so this command starts directly in the running phase.
        prepared->core = std::make_unique<ExecutionCore>(command_limits(command),
                                                         Millis{static_cast<std::int64_t>(started)});
        prepared->core->result().security = security_policy_id(config_);
        // Durable acceptance is the boundary after which this request cannot be rerun.
        records_->accept(*prepared->accepted);
        execution_ = std::move(prepared);
        lifecycle_.transition(SessionState::running);
        auto &channel = launcher_->init_channel();
        try {
            channel.send(StdinHandle{index}, execution_->input.read.get());
            channel.send(StdoutHandle{index}, execution_->output.write.get());
            channel.send(StderrHandle{index}, execution_->error.write.get());
            channel.send(Command{index, dispatch});
            execution_->release_child_ends();
            observe_core(execution_->core->env_ready(Millis{static_cast<std::int64_t>(started)}));
            persist();
        } catch (...) {
            execution_->release_child_ends();
            close("command_dispatch_failed");
            throw;
        }
        return current();
    }
    Json write_stdin(const WriteStdinRequest &input) {
        if (!execution_ || !execution_->accepted ||
            execution_->accepted->index.str() != input.command_id.str())
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
    // Typed dispatch: the wire shape is decoded once, before this point, so every
    // branch here works on validated business types.
    //
    // The return value is deliberately Json: it is the *protocol's* vocabulary,
    // which proto/session_v1.proto already fixes (metrics included, as an opaque
    // blob), not a business type that would have to be invented and kept in step.
    // Durable answers are rendered from records by session_wire; see REDESIGN §4.1.
    Json handle(const SessionRequest &request) {
        const auto check_active = [&](const CommandIndex &command) {
            if (!execution_ || !execution_->accepted ||
                execution_->accepted->index.str() != command.str())
                throw SessionError("COMMAND_NOT_ACTIVE", "command is no longer active");
        };
        return std::visit(
            [&](const auto &value) -> Json {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Status>) {
                    return snapshot();
                } else if constexpr (std::is_same_v<T, RequestStatusQuery>) {
                    return command_reply(records_->request_status(value.key));
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
                        // The stop ladder is the core's; it decides when to ask and
                        // when asking has failed. See observe_core().
                        observe_core(execution_->core->stop_requested(
                            StopIntent::cancel, Millis{static_cast<std::int64_t>(monotonic_ms())}));
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
                    auto report = records_->result(value.command);
                    // Accepted with no outcome yet is not a terminal answer.
                    if (!report)
                        return {{"state", "pending"}};
                    return session_wire::command_report_to_json(*report);
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
                if (!execution_ || !execution_->accepted)
                    throw std::runtime_error("command result mismatch");
                if (done->command != execution_->accepted->index)
                    throw std::runtime_error("command result mismatch");
                const Millis now{static_cast<std::int64_t>(monotonic_ms())};
                // From here the group is reported gone, so a later signal request
                // would address whatever runs next; observe_core() drops those.
                execution_->done = true;
                // One message carries three facts. The reason goes first: "we asked
                // it to stop" must decide the outcome before the kernel's SIGTERM is
                // mistaken for the target's own death.
                switch (done->stop) {
                case CommandStop::cancelled:
                    observe_core(execution_->core->stop_requested(StopIntent::cancel, now));
                    break;
                case CommandStop::timeout:
                    observe_core(execution_->core->stop_requested(StopIntent::timeout, now));
                    break;
                case CommandStop::session_closed:
                    observe_core(execution_->core->stop_requested(StopIntent::session_closed, now));
                    break;
                case CommandStop::startup_failure:
                    observe_core(execution_->core->start_failed(
                        "exec failed", done->exec_errno.value_or(0), now));
                    break;
                case CommandStop::supervision_failure:
                    observe_core(
                        execution_->core->supervisor_failed("command supervision failed", now));
                    break;
                case CommandStop::main_exit:
                    break;
                }
                observe_core(execution_->core->main_exited(done->exit_code, done->signal, now));
                // init only reports a command done after it observed the set empty.
                observe_core(execution_->core->descendants_empty(now));
                if (!lifecycle_.stopping())
                    lifecycle_.transition(SessionState::draining);
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
                if (execution_) {
                    observe_outputs(monotonic_ms());
                    if (execution_ && execution_->core->finished()) {
                        if (sampler_)
                            sample_metrics(monotonic_ms(), true);
                        // The core's result *is* the durable result: one model for a
                        // single run and a session command, rendered once at the
                        // storage and wire boundaries.
                        records_->finish(FinishedCommand{execution_->accepted->index,
                                                         execution_->core->result(),
                                                         std::chrono::system_clock::now()});
                        last_ = session_wire::command_report_to_json(*records_->result(execution_->accepted->index));
                        execution_.reset();
                        if (!lifecycle_.stopping())
                            lifecycle_.transition(SessionState::idle);
                        idle_ = monotonic_ms();
                        persist();
                    }
                }
                if (execution_ && !execution_->core->finished())
                    observe_core(execution_->core->tick(
                        Millis{static_cast<std::int64_t>(monotonic_ms())}));
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
        auto recovery = records_->recover(snapshot());
        auto result = recovery.session;
        if (recovery.last)
            result["last"] = session_wire::command_report_to_json(*recovery.last);
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
