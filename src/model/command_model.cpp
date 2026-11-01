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
// The session's own command model lived here. It is gone: a command is now
// supervised by core/supervisor's ExecutionCore and its result is an
// ExecutionResult, so there is no second answer to "what happened".
} // namespace sandbox
