#include "sandbox/command_model.hpp"
#include <limits>
namespace sandbox {
namespace {
std::uint64_t positive(const Json &n) {
    if ((!n.is_number_unsigned() && !n.is_number_integer()) ||
        (n.is_number_integer() && !n.is_number_unsigned() && n.get<std::int64_t>() <= 0))
        throw SessionError("INVALID_REQUEST", "timeout must be a positive integer");
    auto value = n.get<std::uint64_t>();
    if (!value || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw SessionError("INVALID_REQUEST", "timeout outside supported range");
    return value;
}
std::optional<int> optional_number(const Json &j, const char *key) {
    const auto &n = j.at(key);
    if (n.is_null())
        return std::nullopt;
    if ((!n.is_number_integer() && !n.is_number_unsigned()) || n < 0 ||
        n > std::numeric_limits<int>::max())
        throw SessionError("INVALID_REQUEST", std::string("invalid exit fact: ") + key);
    return n.get<int>();
}
} // namespace
ExecuteRequest ExecuteRequest::parse(const Json &j) {
    require_keys(j, {"op", "request_id", "argv", "cwd", "env", "wall_ms"}, "execute");
    if (j.at("op") != "execute")
        throw SessionError("INVALID_REQUEST", "expected execute request");
    ExecuteRequest request;
    if (j.contains("request_id"))
        request.request_id = RequestId::parse(j.at("request_id").get<std::string>());
    request.argv = j.at("argv").get<std::vector<std::string>>();
    if (request.argv.empty() || request.argv.front().empty() || request.argv.front().front() != '/')
        throw SessionError("INVALID_REQUEST", "argv requires an absolute executable");
    for (const auto &arg : request.argv)
        if (arg.find('\0') != std::string::npos)
            throw SessionError("INVALID_REQUEST", "NUL in argv");
    if (j.contains("cwd"))
        request.cwd = j.at("cwd").get<std::string>();
    if (j.contains("env"))
        request.env = j.at("env").get<std::map<std::string, std::string>>();
    if (j.contains("wall_ms"))
        request.timeout = std::chrono::milliseconds(positive(j.at("wall_ms")));
    return request;
}
ExecuteRequest ExecuteRequest::from_fields(std::optional<std::string> id,
                                           std::vector<std::string> args,
                                           std::optional<std::string> directory,
                                           std::optional<std::map<std::string, std::string>> environment,
                                           std::optional<std::uint64_t> wall_ms) {
    ExecuteRequest request;
    if (id) request.request_id = RequestId::parse(std::move(*id));
    if (args.empty() || args.front().empty() || args.front().front() != '/')
        throw SessionError("INVALID_REQUEST", "argv requires an absolute executable");
    for (const auto &arg : args)
        if (arg.find('\0') != std::string::npos)
            throw SessionError("INVALID_REQUEST", "NUL in argv");
    request.argv = std::move(args);
    if (directory) request.cwd = std::move(*directory);
    request.env = std::move(environment);
    if (wall_ms) {
        if (!*wall_ms || *wall_ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw SessionError("INVALID_REQUEST", "timeout outside supported range");
        request.timeout = std::chrono::milliseconds(*wall_ms);
    }
    return request;
}
Json ExecuteRequest::json() const {
    Json j = {{"op", "execute"}, {"argv", argv}};
    if (request_id)
        j["request_id"] = request_id->str();
    if (cwd)
        j["cwd"] = cwd->string();
    if (env)
        j["env"] = *env;
    if (timeout)
        j["wall_ms"] = timeout->count();
    return j;
}
bool ExecuteRequest::same_intent(const ExecuteRequest &other) const {
    if (request_id != other.request_id || argv != other.argv || env != other.env ||
        timeout != other.timeout || cwd.has_value() != other.cwd.has_value())
        return false;
    return !cwd || cwd->native() == other.cwd->native();
}
const char *stop_reason_name(StopReason reason) {
    switch (reason) {
    case StopReason::exited:
        return "main_exited";
    case StopReason::cancelled:
        return "cancelled";
    case StopReason::timed_out:
        return "timeout";
    case StopReason::session_closed:
        return "session_closed";
    case StopReason::startup_failed:
        return "startup_failure";
    case StopReason::supervision_failed:
        return "host_error";
    }
    throw std::logic_error("invalid stop reason");
}
CommandFacts CommandFacts::parse(const Json &j) {
    auto name = j.at("stop_reason").get<std::string>();
    std::optional<StopReason> reason;
    for (auto value :
         {StopReason::exited, StopReason::cancelled, StopReason::timed_out,
          StopReason::session_closed, StopReason::startup_failed, StopReason::supervision_failed})
        if (name == stop_reason_name(value))
            reason = value;
    if (!reason)
        throw SessionError("INVALID_REQUEST", "unknown command stop reason");
    const auto &elapsed = j.at("execution_ms");
    if (!elapsed.is_number_integer() || elapsed < 0 ||
        elapsed > std::numeric_limits<std::int64_t>::max())
        throw SessionError("INVALID_REQUEST", "invalid elapsed time");
    return {CommandId::parse(j.at("command_id").get<std::string>()),
            optional_number(j, "exit_code"),
            optional_number(j, "signal"),
            optional_number(j, "exec_errno"),
            *reason,
            std::chrono::milliseconds(elapsed.get<std::int64_t>())};
}
void CommandRecord::apply(CommandFacts value) {
    if (facts || phase != CommandPhase::running || value.id != id)
        throw std::logic_error("duplicate or mismatched command facts");
    facts = std::move(value);
    phase = CommandPhase::draining;
}
void CommandRecord::finish(StreamReport stdout_report, StreamReport stderr_report,
                           Json observations) {
    if (phase != CommandPhase::draining || !facts || !stdout_report.eof || !stderr_report.eof ||
        !stdout_report.flushed || !stderr_report.flushed)
        throw std::logic_error("command not fully drained and written");
    out = std::move(stdout_report);
    err = std::move(stderr_report);
    metrics = std::move(observations);
    phase = CommandPhase::finished;
}
Json CommandRecord::json() const {
    Json value = {{"command_id", id.str()},
                  {"argv", argv},
                  {"cwd", cwd.string()},
                  {"state", phase == CommandPhase::finished ? "finished" : "running"}};
    if (request_id)
        value["request_id"] = request_id->str();
    if (facts) {
        auto number = [](std::optional<int> n) { return n ? Json(*n) : Json(nullptr); };
        value.update({{"exit_code", number(facts->exit_code)},
                      {"signal", number(facts->signal)},
                      {"exec_errno", number(facts->exec_errno)},
                      {"stop_reason", stop_reason_name(facts->reason)},
                      {"execution_ms", facts->elapsed.count()}});
    }
    if (phase == CommandPhase::finished) {
        auto stream = [](const StreamReport &s) {
            auto j = s.json();
            j["seen"] = s.saved + s.discarded;
            j["stored"] = s.saved;
            return j;
        };
        value["stdout"] = stream(out);
        value["stderr"] = stream(err);
        value["metrics"] = metrics;
    }
    return value;
}
} // namespace sandbox
