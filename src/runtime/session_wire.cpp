#include "sandbox/session_wire.hpp"
#include "sandbox/command_model.hpp"
#include "session_v1.pb.h"
#include <limits>
namespace sandbox::session_wire {
namespace pb = ::sandbox::session::v1;
namespace {
template <class Message> std::string pack(const Message &message) {
    std::string data(magic);
    if (!message.AppendToString(&data) || data.size() > session_message_limit)
        throw SessionError("INVALID_REQUEST", "encoded packet exceeds 16KiB");
    return data;
}
template <class Message> Message unpack(std::string_view packet) {
    if (packet.size() > session_message_limit || !packet.starts_with(magic))
        throw SessionError("INVALID_REQUEST", "invalid protocol framing");
    Message result;
    auto body = packet.substr(magic.size());
    if (!result.ParseFromArray(body.data(), static_cast<int>(body.size())))
        throw SessionError("INVALID_REQUEST", "invalid protobuf message");
    if (result.protocol_version() != 1)
        throw SessionError("UNSUPPORTED_VERSION", "supported protobuf protocol version: 1");
    return result;
}
// Business types are the only place a validation rule lives; this only turns a
// rejected request into the error the protocol already understands.
template <class F> auto business(F f) -> decltype(f()) {
    try {
        return f();
    } catch (const RequestError &failure) {
        throw SessionError(failure.code, failure.what());
    }
}
CommandIndex command_of(const std::string &text) {
    auto index = CommandIndex::parse(text);
    if (!index)
        throw SessionError("INVALID_REQUEST", "invalid command ID");
    return *index;
}
RequestKey key_of(const std::string &text) {
    auto key = RequestKey::parse(text);
    if (!key)
        throw SessionError("INVALID_REQUEST", "invalid request_id");
    return *key;
}
// Every branch returns a fully validated business value: SessionRequest::create is
// the only way to build one, so a half-checked request cannot escape this function.
SessionOperation operation_of(const pb::SessionRequest &m) {
    switch (m.operation_case()) {
    case pb::SessionRequest::kExecute: {
        const auto &x = m.execute();
        if (m.application_version() == 2 && !x.has_request_id())
            throw SessionError("INVALID_REQUEST", "protocol v2 execute requires request_id");
        std::optional<RequestKey> key;
        if (x.has_request_id())
            key = key_of(x.request_id());
        std::optional<std::filesystem::path> cwd;
        if (x.has_cwd())
            cwd = std::filesystem::path(x.cwd());
        std::optional<std::map<std::string, std::string>> env;
        if (x.has_env()) {
            env.emplace();
            for (const auto &entry : x.env().values())
                env->emplace(entry.first, entry.second);
        }
        std::optional<std::chrono::milliseconds> timeout;
        if (x.has_wall_ms()) {
            // Guard the cast: wall_ms above int64 would be undefined, not slow.
            if (x.wall_ms() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw SessionError("INVALID_REQUEST", "wall_ms outside supported range");
            timeout = std::chrono::milliseconds(static_cast<std::int64_t>(x.wall_ms()));
        }
        std::vector<std::string> argv(x.argv().begin(), x.argv().end());
        return Execute{business([&] {
            return ExecutionRequest::create(std::move(argv), std::move(cwd), std::move(env),
                                            timeout, std::move(key));
        })};
    }
    case pb::SessionRequest::kStatus:
        return Status{};
    case pb::SessionRequest::kClose:
        return Close{};
    case pb::SessionRequest::kCancel: {
        const auto &x = m.cancel();
        std::optional<CommandIndex> command;
        if (x.has_command_id())
            command = command_of(x.command_id());
        return Cancel{command};
    }
    case pb::SessionRequest::kStdinWrite: {
        const auto &x = m.stdin_write();
        if (x.data().size() > max_stdin_chunk)
            throw SessionError("INVALID_REQUEST", "stdin chunk exceeds 4096 bytes");
        return StdinWrite{command_of(x.command_id()), x.data()};
    }
    case pb::SessionRequest::kStdinClose:
        return StdinClose{command_of(m.stdin_close().command_id())};
    case pb::SessionRequest::kOutput: {
        const auto &x = m.output();
        Stream stream = Stream::standard_output;
        if (x.stream() == pb::STDERR)
            stream = Stream::standard_error;
        else if (x.stream() != pb::STDOUT)
            throw SessionError("INVALID_REQUEST", "invalid output stream");
        return ReadOutput{command_of(x.command_id()), stream, x.offset()};
    }
    case pb::SessionRequest::kResult:
        return ReadResult{command_of(m.result().command_id())};
    case pb::SessionRequest::kRequestStatus:
        return RequestStatusQuery{key_of(m.request_status().request_id())};
    case pb::SessionRequest::kWorkspace: {
        const auto &w = m.workspace();
        switch (w.operation_case()) {
        case pb::WorkspaceRequest::kSnapshot:
            return Workspace{WorkspaceSnapshot{w.snapshot().source(), w.snapshot().name()}};
        case pb::WorkspaceRequest::kManifest:
            return Workspace{WorkspaceManifest{}};
        case pb::WorkspaceRequest::kExportFiles:
            return Workspace{WorkspaceExport{w.export_files().destination()}};
        default:
            throw SessionError("UNKNOWN_OPERATION", "missing workspace operation");
        }
    }
    default:
        throw SessionError("UNKNOWN_OPERATION", "missing or unsupported session operation");
    }
}
} // namespace

DecodedRequest decode_session_request(std::string_view packet) {
    auto m = unpack<pb::SessionRequest>(packet);
    if (m.application_version() != 1 && m.application_version() != 2)
        throw SessionError("UNSUPPORTED_VERSION", "supported protocol versions: 1, 2");
    return DecodedRequest{SessionRequest::create(operation_of(m)), m.application_version()};
}

std::string_view operation_name(const SessionOperation &operation) noexcept {
    switch (operation.index()) {
    case 0:
        return "execute";
    case 1:
        return "status";
    case 2:
        return "close";
    case 3:
        return "cancel";
    case 4:
        return "stdin";
    case 5:
        return "stdin-close";
    case 6:
        return "output";
    case 7:
        return "result";
    case 8:
        return "request-status";
    default:
        return "files";
    }
}

Json session_request_to_json(const SessionRequest &request, std::uint32_t application_version) {
    Json j = {{"protocol_version", application_version}};
    std::visit(
        [&j](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Execute>) {
                const auto &r = value.request;
                j["op"] = "execute";
                j["argv"] = r.argv();
                if (r.key())
                    j["request_id"] = r.key()->str();
                if (r.cwd())
                    j["cwd"] = r.cwd()->string();
                if (r.env()) {
                    j["env"] = Json::object();
                    for (const auto &[name, content] : *r.env())
                        j["env"][name] = content;
                }
                if (r.timeout())
                    j["wall_ms"] = r.timeout()->count();
            } else if constexpr (std::is_same_v<T, Status>) {
                j["op"] = "status";
            } else if constexpr (std::is_same_v<T, Close>) {
                j["op"] = "close";
            } else if constexpr (std::is_same_v<T, Cancel>) {
                j["op"] = "cancel";
                if (value.command)
                    j["command_id"] = value.command->str();
            } else if constexpr (std::is_same_v<T, StdinWrite>) {
                j["op"] = "stdin";
                j["command_id"] = value.command.str();
                j["bytes"] = std::vector<unsigned char>(value.bytes.begin(), value.bytes.end());
            } else if constexpr (std::is_same_v<T, StdinClose>) {
                j["op"] = "stdin-close";
                j["command_id"] = value.command.str();
            } else if constexpr (std::is_same_v<T, ReadOutput>) {
                j["op"] = "output";
                j["command_id"] = value.command.str();
                j["stream"] = value.stream == Stream::standard_output ? "stdout" : "stderr";
                j["offset"] = value.offset;
            } else if constexpr (std::is_same_v<T, ReadResult>) {
                j["op"] = "result";
                j["command_id"] = value.command.str();
            } else if constexpr (std::is_same_v<T, RequestStatusQuery>) {
                j["op"] = "request-status";
                j["request_id"] = value.key.str();
            } else {
                std::visit(
                    [&j](const auto &inner) {
                        using W = std::decay_t<decltype(inner)>;
                        j["op"] = "files";
                        if constexpr (std::is_same_v<W, WorkspaceSnapshot>) {
                            j["request"] = {
                                {"op", "snapshot"}, {"source", inner.source}, {"name", inner.name}};
                        } else if constexpr (std::is_same_v<W, WorkspaceManifest>) {
                            j["request"] = {{"op", "manifest"}};
                        } else {
                            j["request"] = {{"op", "export"}, {"destination", inner.destination}};
                        }
                    },
                    value.operation);
            }
        },
        request.operation());
    return j;
}

Json decode_request(std::string_view packet) {
    const auto decoded = decode_session_request(packet);
    return session_request_to_json(decoded.request, decoded.application_version);
}

std::string encode_request(const Json &request) {
    auto j = validate_session_request(request);
    pb::SessionRequest m; m.set_protocol_version(1); m.set_application_version(request.value("protocol_version", 1));
    auto op = j.at("op").get<std::string>();
    if (op == "execute") {
        // Validate JSON before conversion so floats, negatives and other invalid
        // values cannot be silently coerced by protobuf scalar setters.
        (void)ExecuteRequest::parse(j);
        auto *x = m.mutable_execute(); if (j.contains("request_id")) x->set_request_id(j.at("request_id").get<std::string>());
        for (const auto &a : j.at("argv")) x->add_argv(a.get<std::string>());
        if (j.contains("cwd")) x->set_cwd(j.at("cwd").get<std::string>());
        if (j.contains("wall_ms")) x->set_wall_ms(j.at("wall_ms").get<std::uint64_t>());
        if (j.contains("env")) { auto *env = x->mutable_env()->mutable_values(); for (auto it = j.at("env").begin(); it != j.at("env").end(); ++it) (*env)[it.key()] = it.value().get<std::string>(); }
    } else if (op == "status") m.mutable_status();
    else if (op == "close") m.mutable_close();
    else if (op == "cancel") { auto *x = m.mutable_cancel(); if (j.contains("command_id")) x->set_command_id(j.at("command_id").get<std::string>()); }
    else if (op == "stdin") {
        auto *x = m.mutable_stdin_write(); x->set_command_id(j.at("command_id").get<std::string>());
        auto bytes = j.at("bytes").get<std::vector<unsigned>>(); if (bytes.size() > 4096) throw SessionError("INVALID_REQUEST", "stdin chunk exceeds 4096 bytes");
        std::string data; for (auto value : bytes) { if (value > 255) throw SessionError("INVALID_REQUEST", "invalid byte"); data.push_back(static_cast<char>(value)); } x->set_data(data);
    } else if (op == "stdin-close") m.mutable_stdin_close()->set_command_id(j.at("command_id").get<std::string>());
    else if (op == "result") m.mutable_result()->set_command_id(j.at("command_id").get<std::string>());
    else if (op == "request-status") m.mutable_request_status()->set_request_id(j.at("request_id").get<std::string>());
    else if (op == "output") {
        auto *x = m.mutable_output(); x->set_command_id(j.at("command_id").get<std::string>());
        auto stream = j.value("stream", "stdout"); if (stream != "stdout" && stream != "stderr") throw SessionError("INVALID_REQUEST", "invalid stream");
        x->set_stream(stream == "stdout" ? pb::STDOUT : pb::STDERR); x->set_offset(j.value("offset", std::uint64_t{0}));
    } else if (op == "files") {
        auto &f = j.at("request"); auto action = f.at("op").get<std::string>(); auto *w = m.mutable_workspace();
        if (action == "snapshot") { require_keys(f, {"op", "source", "name"}, "session snapshot"); w->mutable_snapshot()->set_source(f.at("source").get<std::string>()); w->mutable_snapshot()->set_name(f.at("name").get<std::string>()); }
        else if (action == "manifest") { require_keys(f, {"op"}, "session manifest"); w->mutable_manifest(); }
        else if (action == "export") { require_keys(f, {"op", "destination"}, "session export"); w->mutable_export_files()->set_destination(f.at("destination").get<std::string>()); }
        else throw SessionError("UNKNOWN_OPERATION", "invalid workspace operation");
    }
    return pack(m);
}
namespace {
void put_stream(const Json &j, pb::StreamSummary *p) {
    p->set_saved_bytes(j.at("saved_bytes").get<std::uint64_t>()); p->set_discarded_bytes(j.at("discarded_bytes").get<std::uint64_t>());
    p->set_eof(j.at("eof").get<bool>()); p->set_flushed(j.at("flushed").get<bool>());
    p->set_queue_peak_bytes(j.at("queue_peak_bytes").get<std::uint64_t>()); p->set_backpressure_events(j.at("backpressure_events").get<std::uint64_t>());
    p->set_failed_bytes(j.at("failed_bytes").get<std::uint64_t>()); p->set_write_error(j.at("write_error").get<std::string>());
    p->set_seen(j.at("seen").get<std::uint64_t>()); p->set_stored(j.at("stored").get<std::uint64_t>());
}
Json get_stream(const pb::StreamSummary &p) {
    return {{"saved_bytes",p.saved_bytes()},{"discarded_bytes",p.discarded_bytes()},{"eof",p.eof()}, {"flushed",p.flushed()},
            {"queue_peak_bytes",p.queue_peak_bytes()},{"backpressure_events",p.backpressure_events()}, {"failed_bytes",p.failed_bytes()},
            {"write_error",p.write_error()},{"seen",p.seen()},{"stored",p.stored()}};
}
void put_command(const Json &j, pb::CommandRecord *p) {
    if (j.contains("command_id")) p->set_command_id(j.at("command_id").get<std::string>());
    if (j.contains("request_id")) p->set_request_id(j.at("request_id").get<std::string>());
    if (j.contains("argv")) { p->set_has_argv(true); for (const auto &a : j.at("argv")) p->add_argv(a.get<std::string>()); }
    if (j.contains("cwd")) p->set_cwd(j.at("cwd").get<std::string>());
    p->set_state(j.at("state").get<std::string>());
    if (j.contains("exit_code")) { p->set_has_exit_code_field(true); if (!j.at("exit_code").is_null()) p->set_exit_code(j.at("exit_code").get<int>()); }
    if (j.contains("signal")) { p->set_has_signal_field(true); if (!j.at("signal").is_null()) p->set_signal(j.at("signal").get<int>()); }
    if (j.contains("exec_errno")) { p->set_has_exec_errno_field(true); if (!j.at("exec_errno").is_null()) p->set_exec_errno(j.at("exec_errno").get<int>()); }
    if (j.contains("stop_reason")) p->set_stop_reason(j.at("stop_reason").get<std::string>());
    if (j.contains("execution_ms")) p->set_execution_ms(j.at("execution_ms").get<std::uint64_t>());
    if (j.contains("stdout")) put_stream(j.at("stdout"),p->mutable_stdout_stream());
    if (j.contains("stderr")) put_stream(j.at("stderr"),p->mutable_stderr_stream());
    if (j.contains("metrics")) p->set_metrics_json(j.at("metrics").dump());
    if (j.contains("outcome_known")) p->set_outcome_known(j.at("outcome_known").get<bool>());
    if (j.contains("schema_version")) p->set_schema_version(j.at("schema_version").get<unsigned>());
}
Json get_command(const pb::CommandRecord &p) {
    Json j = {{"state",p.state()}};
    if (!p.command_id().empty()) j["command_id"] = p.command_id();
    if (p.has_request_id()) j["request_id"] = p.request_id();
    if (p.has_argv()) j["argv"] = std::vector<std::string>(p.argv().begin(),p.argv().end());
    if (p.has_cwd()) j["cwd"] = p.cwd();
    if (p.has_stop_reason()) j["stop_reason"] = p.stop_reason();
    if (p.has_exit_code_field()) j["exit_code"] = p.has_exit_code() ? Json(p.exit_code()) : Json(nullptr);
    if (p.has_signal_field()) j["signal"] = p.has_signal() ? Json(p.signal()) : Json(nullptr);
    if (p.has_exec_errno_field()) j["exec_errno"] = p.has_exec_errno() ? Json(p.exec_errno()) : Json(nullptr);
    if (p.has_execution_ms()) j["execution_ms"] = p.execution_ms();
    if (p.has_stdout_stream()) j["stdout"] = get_stream(p.stdout_stream());
    if (p.has_stderr_stream()) j["stderr"] = get_stream(p.stderr_stream());
    if (p.has_metrics_json()) j["metrics"] = Json::parse(p.metrics_json());
    if (p.has_outcome_known()) j["outcome_known"] = p.outcome_known();
    if (p.has_schema_version()) j["schema_version"] = p.schema_version();
    return j;
}
}
std::string encode_response(std::string_view operation, const Json &answer) {
    pb::SessionResponse m; m.set_protocol_version(1);
    if (operation == "execute" || operation == "result" || operation == "request-status") put_command(answer,m.mutable_command());
    else if (operation == "status") {
        auto *x = m.mutable_status(); x->set_run_id(answer.at("run_id").get<std::string>());
        x->set_schema_version(answer.at("schema_version").get<unsigned>()); x->set_session_protocol_version(answer.at("protocol_version").get<unsigned>());
        x->set_session_state(answer.at("session_state").get<std::string>()); x->set_commands(answer.at("commands").get<std::uint64_t>());
        if (!answer.at("current").is_null()) put_command(answer.at("current"),x->mutable_current());
        if (!answer.at("last").is_null()) put_command(answer.at("last"),x->mutable_last());
        x->set_metrics_json(answer.at("metrics").dump());
    } else if (operation == "stdin") m.mutable_input()->set_accepted_bytes(answer.at("accepted_bytes").get<std::uint64_t>());
    else if (operation == "output") {
        auto *x=m.mutable_output(); x->set_offset(answer.at("offset").get<std::uint64_t>()); x->set_next_offset(answer.at("next_offset").get<std::uint64_t>());
        auto bytes=answer.at("bytes").get<std::vector<unsigned char>>(); x->set_data(bytes.data(),bytes.size()); x->set_finished(answer.at("finished").get<bool>());
    } else if (operation == "close" || operation == "cancel" || operation == "stdin-close") {
        auto *x=m.mutable_acknowledgement(); if (answer.contains("status")) x->set_status(answer.at("status").get<std::string>());
        if (answer.contains("active")) x->set_active(answer.at("active").get<bool>()); if (answer.contains("closed")) x->set_closed(answer.at("closed").get<bool>());
    } else if (operation == "files") {
        auto *x=m.mutable_workspace(); x->set_bytes(answer.at("bytes").get<std::uint64_t>()); x->set_entry_count(answer.at("entry_count").get<std::uint64_t>());
        x->set_manifest_truncated(answer.at("manifest_truncated").get<bool>());
        if (answer.contains("complete")) x->set_complete(answer.at("complete").get<bool>());
        if (answer.contains("destination")) x->set_destination(answer.at("destination").get<std::string>());
        for (const auto &item : answer.at("entries")) { auto *e=x->add_entries(); e->set_path(item.at("path").get<std::string>()); e->set_type(item.at("type").get<std::string>());
            if (item.contains("size")) e->set_size(item.at("size").get<std::uint64_t>()); if (item.contains("sha256")) e->set_sha256(item.at("sha256").get<std::string>()); }
    } else throw SessionError("UNKNOWN_OPERATION", "unsupported response operation");
    return pack(m);
}
std::string encode_error(const SessionError &error) {
    pb::SessionResponse m; m.set_protocol_version(1); auto *e=m.mutable_error(); e->set_code(error.code); e->set_message(error.what()); e->set_retryable(error.retryable); return pack(m);
}
Json decode_response(std::string_view packet) {
    auto m=unpack<pb::SessionResponse>(packet);
    switch(m.outcome_case()) {
    case pb::SessionResponse::kError: throw SessionError(m.error().code(),m.error().message(),m.error().retryable());
    case pb::SessionResponse::kCommand: return get_command(m.command());
    case pb::SessionResponse::kStatus: {
        const auto &x=m.status(); return {{"run_id",x.run_id()},{"schema_version",x.schema_version()},{"protocol_version",x.session_protocol_version()},
            {"session_state",x.session_state()},{"commands",x.commands()},{"current",x.has_current()?get_command(x.current()):Json(nullptr)},
            {"last",x.has_last()?get_command(x.last()):Json(nullptr)},{"metrics",Json::parse(x.metrics_json())}};
    }
    case pb::SessionResponse::kInput: return {{"accepted_bytes",m.input().accepted_bytes()}};
    case pb::SessionResponse::kOutput: { const auto &x=m.output(); return {{"offset",x.offset()},{"next_offset",x.next_offset()},
        {"bytes",std::vector<unsigned char>(x.data().begin(),x.data().end())},{"finished",x.finished()}}; }
    case pb::SessionResponse::kAcknowledgement: { const auto &x=m.acknowledgement(); Json j=Json::object(); if (!x.status().empty()) j["status"]=x.status();
        if (x.has_active()) j["active"]=x.active(); if (x.has_closed()) j["closed"]=x.closed(); return j; }
    case pb::SessionResponse::kWorkspace: { const auto &x=m.workspace(); Json entries=Json::array(); for (const auto &e : x.entries()) {
        Json item={{"path",e.path()},{"type",e.type()}}; if (e.has_size()) item["size"]=e.size(); if (e.has_sha256()) item["sha256"]=e.sha256(); entries.push_back(item); }
        Json j={{"bytes",x.bytes()},{"entries",entries},{"entry_count",x.entry_count()},{"manifest_truncated",x.manifest_truncated()}};
        if (x.has_complete()) j["complete"]=x.complete(); if (x.has_destination()) j["destination"]=x.destination(); return j; }
    default: throw SessionError("INVALID_RESPONSE", "missing or unsupported response");
    }
}
} // namespace sandbox::session_wire
