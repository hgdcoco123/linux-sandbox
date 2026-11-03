#include "sandbox/runtime/command_execution.hpp"
#include "sandbox/core/ops_json.hpp"
#include "sandbox/platform/event_loop.hpp"
#include "sandbox/runtime/interactive.hpp"
#include "sandbox/storage/artifacts.hpp"
#include "sandbox/isolation/isolation.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include "sandbox/runtime/session.hpp"
#include "sandbox/runtime/session_policy.hpp"
#include "sandbox/runtime/session_state.hpp"
#include "sandbox/core/prepare.hpp"
#include "sandbox/storage/session_store.hpp"
#include "sandbox/runtime/session_wire.hpp"
#include "sandbox/storage/snapshot.hpp"
#include <cstdlib>
#include <cstring>
#include "sandbox/storage/workspace_service.hpp"
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
    fs::path rootfs_;
    // Undo failures from the preparation journal, recorded apart from the reason the
    // session could not start.
    std::vector<CleanupFailure> prepare_rollback_failures_;
    bool preparation_failed_ = false;
    // Quota workspace: the tree lives in a tmpfs inside the sandbox, the host keeps
    // a published copy, and data crosses through `transfer_`.
    bool quota_workspace_ = false;
    fs::path transfer_;
    std::uint64_t export_sequence_ = 0;
    bool export_attempted_ = false, export_ok_ = false;
    // Visibility and durability are separate facts: a published tree that could not be
    // synced is visible now but may come back as the old one after a crash.
    bool export_durable_ = false;
    std::string export_durability_error_;
    // Where the previous host copy was left when durability could not be confirmed.
    fs::path export_retained_previous_;
    bool export_pending_ = false;
    // One transfer outstanding at a time. A transfer that timed out leaves its copy
    // running inside the sandbox, so the channel is not reusable until that request
    // answers - and a late answer must not be read as the answer to a new one.
    std::uint64_t transfer_sequence_ = 0;
    bool transfer_unusable_ = false;
    std::string transfer_quarantine_reason_;
    bool stale_reply_seen_ = false;
    // Waiting for a copy must not make the supervisor deaf: the signal source is kept
    // so a stop request can be forwarded to the copy itself (init aborts on STOP), and
    // remembered so the outer loop still closes the session once the wait unwinds.
    SignalSource *signals_ = nullptr;
    bool signal_pending_ = false;
    bool stop_during_transfer_ = false;
    std::uint64_t export_bytes_ = 0;
    std::uint32_t export_entries_ = 0;
    std::string export_error_;
    SessionLifecycle lifecycle_;
    // A session used to leave only result.json behind, which says nothing about the
    // order of channel events; every diagnosis cost a full reproduction. Same file a
    // single run writes, same rules: info for the sequence, warn for what went wrong.
    std::shared_ptr<spdlog::logger> logger_;
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
    // An interrupted copy really did stop, so the channel is still usable - unlike a
    // timeout, where the copy may be running. The distinction is carried by init's
    // answer as a typed reason (`TransferStop`).
    // One transfer at a time, and a transfer that timed out poisons the channel: the
    // copy inside the sandbox may still be running, so a new request could be
    // answered by the *old* reply. A late answer is recognised by its sequence and
    // discarded; until it arrives, transfers fail fast instead of guessing.
    Transferred await_transfer(std::uint64_t sequence) {
        const auto deadline = monotonic_ms() + transfer_wait_ms();
        for (;;) {
            // The copy runs inside the sandbox while the supervisor waits here. Waiting
            // must not make it deaf: a stop request is forwarded as STOP, which is what
            // makes init abort the copy (InterruptOnStop) instead of finishing it for
            // nobody. The request is remembered because draining the signal consumes it;
            // the outer loop still has to close the session afterwards.
            if (signals_ && signals_->drain_cancel()) {
                signal_pending_ = true;
                stop_during_transfer_ = true;
                try {
                    if (init_open_)
                        launcher_->init_channel().send(Stop{});
                } catch (const std::exception &) {
                    // init is already gone; there is nothing left to abort.
                }
            }
            const auto now = monotonic_ms();
            if (closed_ != 0 && now - closed_ > config_.limits.grace_ms + 5000) {
                // The close deadline does not pause while a copy runs: the session has
                // promised to be gone by then, and a copy may still be running.
                transfer_unusable_ = true;
                transfer_quarantine_reason_ =
                    "the session close deadline expired while the sandbox was copying";
                throw SessionError("TRANSFER_TIMEOUT", transfer_quarantine_reason_);
            }
            std::optional<ControlEnvelope> frame;
            try {
                frame = launcher_->init_channel().receive(true);
            } catch (const ChannelClosed &) {
                init_open_ = false;
                throw SessionError("TRANSFER_FAILED", "the sandbox exited during the transfer");
            }
            if (frame) {
                if (auto *answer = std::get_if<Transferred>(&frame->message)) {
                    if (answer->sequence != sequence) {
                        // This is the answer to an abandoned request. Forget the
                        // quarantine it caused and keep waiting for our own.
                        if (!stale_reply_seen_) {
                            stale_reply_seen_ = true;
                            errors_.push_back("stale transfer reply discarded (sequence " +
                                              std::to_string(answer->sequence) + ")");
                        }
                        continue;
                    }
                    return *answer;
                }
                throw SessionError("TRANSFER_FAILED", "unexpected message from the sandbox");
            }
            if (!init_open_)
                throw SessionError("TRANSFER_FAILED", "the sandbox exited during the transfer");
            if (now >= deadline) {
                // The copy may still be running inside the sandbox, so the channel is
                // not reusable: from here on every transfer fails fast.
                transfer_unusable_ = true;
                transfer_quarantine_reason_ =
                    "a previous transfer timed out; the sandbox may still be copying";
                throw SessionError("TRANSFER_TIMEOUT",
                                   "the sandbox did not answer within " +
                                       std::to_string(transfer_wait_ms()) +
                                       "ms; the copy may still be running");
            }
            pollfd wait{launcher_->init_channel().fd(), POLLIN, 0};
            ::poll(&wait, 1, 20);
        }
    }
    // Tests need the timeout, so it is adjustable; production keeps a generous
    // default because a large workspace legitimately takes a while to copy.
    static std::uint64_t transfer_wait_ms() {
        if (const char *configured = ::getenv("SBX_TRANSFER_WAIT_MS")) {
            char *end = nullptr;
            const auto value = std::strtoull(configured, &end, 10);
            // Zero is meaningful: it gives the sandbox no time at all, which is how
            // the timeout path is tested without depending on how fast a copy is.
            // Anything that is not a plain number keeps the production default.
            if (end && *end == '\0' && end != configured)
                return value;
        }
        return 30000;
    }
    // Ask the sandbox for its workspace and publish what it hands over. Only legal
    // with no command running: the core's completion is what proves the target and
    // its descendants are gone.
    void sync_workspace() {
        if (!quota_workspace_ || !launcher_)
            return;
        if (!current().is_null())
            throw SessionError("SESSION_BUSY",
                               "the workspace cannot be read while a command runs");
        if (transfer_unusable_)
            throw SessionError("TRANSFER_UNUSABLE", transfer_quarantine_reason_);
        const auto sequence = ++transfer_sequence_;
        const auto tag = "out-" + std::to_string(sequence);
        export_attempted_ = true;
        export_ok_ = false;
        export_error_.clear();
        export_durable_ = false;
        export_durability_error_.clear();
        export_retained_previous_.clear();
        export_bytes_ = 0;
        export_entries_ = 0;
        std::error_code failure;
        fs::remove_all(transfer_ / tag, failure);
        launcher_->init_channel().send(ExportTree{tag, sequence});
        Transferred answer;
        try {
            answer = await_transfer(sequence);
        } catch (const std::exception &failure) {
            export_error_ = failure.what();
            throw;
        }
        if (!answer.ok) {
            export_error_ = answer.error;
            // No quarantine here: an interrupted copy stopped, so the next transfer may
            // use the channel. Only a timeout leaves a copy running.
            throw SessionError(
                answer.stop == TransferStop::interrupted ? "TRANSFER_INTERRUPTED" : "EXPORT_FAILED",
                answer.error);
        }
        export_bytes_ = answer.bytes;
        export_entries_ = answer.entries;
        // Publish through the same implementation the single run uses: exchange, durable
        // swap, reclaim the previous copy (see publish_tree_over). The previous good
        // copy stays in place until the swap actually happens.
        try {
            auto outcome =
                publish_tree_over(transfer_ / tag, store_->directory() / "workspace",
                                  config_.workspace);
            export_ok_ = true;
            // Visibility and durability are reported separately: a published tree that
            // could not be synced is visible now but may come back as the old one after
            // a crash, and the previous copy was deliberately kept for that case.
            export_durable_ = outcome.durability_confirmed;
            export_durability_error_ = outcome.durability_error;
            export_retained_previous_ = outcome.retained_previous_path;
            if (!outcome.cleanup_error.empty())
                errors_.push_back(outcome.cleanup_error);
        } catch (const std::exception &e) {
            export_ok_ = false;
            export_error_ = e.what();
            throw SessionError("EXPORT_FAILED", export_error_);
        }
    }
    // Hand one staged upload to the sandbox, then publish the same tree as the host
    // copy of it.
    void import_workspace(const std::string &name) {
        if (!quota_workspace_ || !launcher_)
            throw SessionError("TRANSFER_FAILED", "workspace transfer is not configured");
        if (transfer_unusable_)
            throw SessionError("TRANSFER_UNUSABLE", transfer_quarantine_reason_);
        const auto sequence = ++transfer_sequence_;
        launcher_->init_channel().send(ImportTree{name, sequence});
        auto answer = await_transfer(sequence);
        if (!answer.ok)
            throw SessionError(
                answer.stop == TransferStop::interrupted ? "TRANSFER_INTERRUPTED" : "IMPORT_FAILED",
                answer.error);
        std::error_code failure;
        fs::rename(transfer_ / "in" / name, store_->directory() / "workspace" / name, failure);
        if (failure)
            throw SessionError("IMPORT_FAILED", "publish upload: " + failure.message());
    }    Json command_reply(const CommandReport &report) const {
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
        if (logger_)
            logger_->info("close: reason={} stopping={}", reason, lifecycle_.stopping());
        if (execution_)
            execution_->input.write.reset();
        // The workspace is handed over while init is still alive: a tmpfs dies with
        // the namespace, and STOP may end it immediately. If a command is still
        // running the tree cannot be read, which is reported rather than silently
        // dropping the artifacts.
        if (quota_workspace_) {
            if (!current().is_null()) {
                // A command is still running: the tree cannot be read yet. Hand it
                // over as soon as init reports it finished, before init exits.
                export_pending_ = true;
            } else if (!stop_during_transfer_) {
                // A stop forwarded during a transfer ends init by design, so there is
                // no environment left to read a workspace from - and asking again would
                // report a second failure for the same stop.
                try {
                    sync_workspace();
                } catch (const std::exception &e) {
                    export_error_ = e.what();
                    if (config_.workspace.keep_on_close)
                        errors_.push_back(std::string("workspace export: ") + e.what());
                }
            }
        }
        if (launcher_ && init_open_) {
            try {
                launcher_->init_channel().send(Stop{});
            } catch (const std::exception &) {
                // init is already gone; there is nothing left to stop.
            }
        }
        persist();
    }
    Json start(const ExecutionRequest &intent) {
        if (auto existing = records_->replay(intent))
            return command_reply(*existing);
        lifecycle_.require_idle();
        if (counter_ >= 1024)
            throw SessionError("COMMAND_LIMIT", "session command count limit 1024 reached");
        RunConfig command;
        try {
            command = resolve_command(config_, intent);
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
        prepared->core->set_security(security_policy_id(config_));
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
    Json write_stdin(const CommandIndex &command, const std::string &bytes) {
        if (!execution_ || !execution_->accepted ||
            execution_->accepted->index.str() != command.str())
            throw SessionError("COMMAND_NOT_ACTIVE", "command is no longer active");
        lifecycle_.require_running();
        if (!execution_->input.write)
            throw std::runtime_error("stdin is closed");
        if (bytes.size() > max_stdin_chunk)
            throw std::runtime_error("stdin chunk exceeds 4096 bytes");
        auto n = ::write(execution_->input.write.get(), bytes.data(), bytes.size());
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
                    // The typed request goes straight through: no round trip through
                    // the JSON-era request type.
                    return start(value.request);
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
                    return write_stdin(value.command, value.bytes);
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
        if (logger_)
            logger_->info("event: {} from {}", control_kind_name(kind_of(envelope.message)),
                          parent ? "launcher" : "init");
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
                // The transfer directory goes over as a descriptor, not a mount and not
                // a path: the sandbox can copy through it without being able to name it,
                // and nothing of it appears in the target's filesystem.
                if (!lifecycle_.stopping() && quota_workspace_) {
                    int transfer = checked(::open(transfer_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC),
                                           "open transfer directory");
                    launcher_->init_channel().send(TransferChannel{}, transfer);
                    ::close(transfer);
                }
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
            } else if (auto *error = std::get_if<InitError>(&envelope.message)) {
                // init reports *why* it gave up; folding that into "unexpected
                // message" throws away the only evidence a session startup has.
                // Recorded by the caller's catch: pushing here would report the
                // same failure twice.
                throw std::runtime_error("init: " + error->message);
            } else if (std::holds_alternative<InitDone>(envelope.message)) {
                done_ = true;
            } else if (auto *late = std::get_if<Transferred>(&envelope.message)) {
                // The answer to a request whose wait was abandoned (a timeout leaves the
                // copy running, so it arrives later). The design promises late answers
                // are discarded by sequence - this is where that promise is kept: the
                // main loop is the only reader left once await_transfer() has given up,
                // and treating one as a protocol error killed a healthy session.
                if (!stale_reply_seen_) {
                    stale_reply_seen_ = true;
                    errors_.push_back("stale transfer reply discarded (sequence " +
                                      std::to_string(late->sequence) + ")");
                }
            } else {
                throw std::runtime_error("init failed: unexpected message");
            }
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
                    if (logger_)
                        logger_->warn("channel closed: parent={} reaped={} done={} stopping={} "
                                      "quarantined={} signal_pending={}",
                                      parent, reaped_, done_, lifecycle_.stopping(),
                                      transfer_unusable_, signal_pending_);
                    if (!(parent ? reaped_ : done_)) {
                        // During a stop the peer exiting is expected - init can be gone
                        // before its INIT_DONE is read, and a close with a STOP still unread
                        // resets the connection and discards that frame. A closed channel
                        // then ends the evidence; it does not make a deliberate close a
                        // failure. Outside a stop the missing fact still means the session
                        // broke, which is what the throw reports.
                        if (!lifecycle_.stopping())
                            throw;
                        if (!parent)
                            done_ = true;
                    }
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
        signals_ = &signals;
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
        logger_ = std::make_shared<spdlog::logger>(
            store_->id(), std::make_shared<spdlog::sinks::basic_file_sink_st>(
                              (store_->directory() / "internal.log").string(), false));
        logger_->set_pattern("[%H:%M:%S.%e] [%l] %v");
        logger_->flush_on(spdlog::level::warn);
        logger_->info("session opened: mode={} quota_workspace={}", config_.mode, quota_workspace_);

        try {
            // Preparation is journaled exactly like a single run: a step that already
            // succeeded is undone in reverse when a later one fails, the reason the
            // session could not start is reported separately from the rollback
            // failures, and the state directory itself is never undone (the report
            // has to live somewhere).
            std::vector<PrepareStepSpec> steps;
            steps.push_back({PrepareStep::state_root,
                             [this] {
                                 rootfs_ = store_->directory() / "rootfs";
                                 auto workspace = store_->directory() / "workspace";
                                 // Quota workspace: the target writes to a tmpfs the
                                 // supervisor cannot see, so the run directory keeps a
                                 // transfer staging area (handed to the sandbox as a
                                 // descriptor) and the published copy.
                                 std::vector<fs::path> layout{rootfs_, workspace};
                                 if (config_.mode == "isolated") {
                                     transfer_ = store_->directory() / "transfer";
                                     layout.push_back(transfer_);
                                     layout.push_back(transfer_ / "in");
                                 }
                                 // One transaction: if any of them cannot be created,
                                 // only what this call created is removed again.
                                 create_directories_or_remove(layout);
                                 if (config_.mode == "isolated") {
                                     config_.workspace_transfer = transfer_;
                                     quota_workspace_ = true;
                                 }
                                 WorkspaceTransfer hooks;
                                 if (quota_workspace_) {
                                     hooks.staging = transfer_ / "in";
                                     hooks.publish = [this] { sync_workspace(); };
                                     hooks.commit = [this](const std::string &name) {
                                         import_workspace(name);
                                     };
                                 }
                                 workspace_ = std::make_unique<WorkspaceService>(
                                     workspace, *records_, config_.workspace, std::move(hooks));
                             },
                             [this] {
                                 // Every directory gets an attempt: one removal that fails
                                 // must not stop the others, the same rule the journal uses
                                 // for rollback. All failures are reported together.
                                 auto failures = remove_all_collect(
                                     {rootfs_, store_->directory() / "workspace", transfer_});
                                 if (!failures.empty())
                                     throw std::runtime_error(failures);
                                 transfer_.clear();
                                 quota_workspace_ = false;
                                 config_.workspace_transfer.clear();
                             }});
            steps.push_back({PrepareStep::control_socket,
                             [this] {
                                 store_->state(RunState::preparing);
                                 server_ = std::make_unique<ControlServer>(
                                     store_->directory() / "control.sock");
                                 persist();
                             },
                             [this] { server_.reset(); }});
            steps.push_back({PrepareStep::cgroup,
                             [this] {
                                 if (config_.mode != "isolated")
                                     return;
                                 group_ = std::make_unique<CgroupV2>(config_.cgroup_root,
                                                                     store_->id(), config_.limits);
                                 sampler_ = std::make_unique<MetricsSampler>(
                                     group_->path(), MetricRegistry::defaults(),
                                     config_.metrics.interval_ms, config_.metrics.history_limit);
                             },
                             [this] {
                                 if (!group_)
                                     return;
                                 try {
                                     group_->kill_all();
                                     group_->remove();
                                 } catch (...) {
                                 }
                                 group_.reset();
                                 sampler_.reset();
                             }});
            steps.push_back(
                {PrepareStep::input_snapshot,
                 [this] { input_snapshots_ = snapshot_inputs(config_, store_->directory()); },
                 [this] {
                     // Same rule as state_root: a snapshot that cannot be removed does
                     // not stop the other ones from being removed.
                     auto failures = remove_all_collect(
                         std::vector<fs::path>(input_snapshots_.begin(), input_snapshots_.end()));
                     input_snapshots_.clear();
                     if (!failures.empty())
                         throw std::runtime_error(failures);
                 }});
            steps.push_back({PrepareStep::launcher,
                             [this] {
                                 // The effective configuration is only final once the
                                 // input snapshot exists, so it is published here,
                                 // immediately before the launcher reads it.
                                 store_->write_json("config.json", config_.to_json());
                                 auto null = open_path("/dev/null", O_RDWR);
                                 launcher_ = std::make_unique<LauncherClient>(
                                     config_, launcher_path_, rootfs_, null.get(), null.get(),
                                     null.get());
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
            PrepareSequence preparation{std::move(steps)};
            auto prepared = preparation.run();
            if (!prepared.ok) {
                preparation_failed_ = true;
                prepare_rollback_failures_ = prepared.rollback_failures;
                // The original reason, not the rollback's: what stopped the session.
                throw std::runtime_error(describe(prepared));
            }
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
                        // The command is finished and its descendants are gone, which
                        // is exactly the moment a deferred handover may run.
                        if (export_pending_ && quota_workspace_ && init_open_) {
                            export_pending_ = false;
                            try {
                                sync_workspace();
                            } catch (const std::exception &e) {
                                export_error_ = e.what();
                                if (config_.workspace.keep_on_close)
                                    errors_.push_back(std::string("workspace export: ") + e.what());
                            }
                        }
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
                // A signal consumed while waiting for a transfer is still a stop
                // request: the wait drained it, so the flag carries it to here.
                bool cancelled = signals.drain_cancel();
                if (signal_pending_) {
                    signal_pending_ = false;
                    cancelled = true;
                }
                if (cancelled) {
                    if (logger_)
                        logger_->info("signal: closing the session");
                    close("signal");
                }
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
            // A session that never started is not the same failure as one that broke
            // while running; the stop reason has to say which.
            reason_ = preparation_failed_ ? "prepare_failed" : "host_error";
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
        if (!rootfs_.empty())
            ::rmdir(rootfs_.c_str());
        if (!lifecycle_.stopping())
            lifecycle_.transition(SessionState::closing);
        lifecycle_.transition(SessionState::closed);
        auto recovery = records_->recover(snapshot());
        auto result = recovery.session;
        if (recovery.last)
            result["last"] = session_wire::command_report_to_json(*recovery.last);
        result["session_state"] = "closed";
        // The workspace is reported as three separate facts: what bounds it, whether
        // the host copy on disk is current, and what the last export did. A target
        // that exited 0 does not make the artifacts delivered.
        result["workspace"] = {
            {"mode", quota_workspace_ ? "tmpfs" : "host_directory"},
            {"quota",
             {{"max_bytes", config_.workspace.max_bytes},
              {"max_entries", config_.workspace.max_entries}}},
            {"export",
             {{"attempted", export_attempted_},
              {"ok", export_ok_},
              {"bytes", export_bytes_},
              {"entries", export_entries_},
              {"error", export_error_},
              // Only meaningful once `ok` is true: whether the visible copy is also on
              // stable storage (a crash may otherwise bring back the previous one).
              {"durable", export_durable_},
              {"durability_error", export_durability_error_},
              {"retained_previous_path", export_retained_previous_.string()}}}};
        result["stop_reason"] = reason_;
        result["errors"] = errors_;
        if (!prepare_rollback_failures_.empty()) {
            auto failures = result.value("cleanup_errors", Json::array());
            for (const auto &failure : prepare_rollback_failures_)
                failures.push_back(std::string("rollback ") + cleanup_stage_name(failure.stage) +
                                   ": " + failure.detail);
            result["cleanup_errors"] = failures;
        }
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
        if (logger_) {
            logger_->info("session finished: stop_reason={} export_ok={}", reason_, export_ok_);
            logger_->flush();
        }
        return result;
    }
};
} // namespace
Json serve_session(RunConfig config, const fs::path &state, const fs::path &launcher) {
    config.persistent = true;
    return SessionController(std::move(config), state, launcher).run();
}
} // namespace sandbox
