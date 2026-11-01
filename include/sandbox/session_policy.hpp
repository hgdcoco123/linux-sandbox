#pragma once
#include "sandbox/command_model.hpp"
#include "sandbox/core/request.hpp"
namespace sandbox {

// The command a session will actually run: the policy's defaults with the caller's
// request applied on top, then validated as a whole. Takes the typed request, so the
// business path never has to spell one twice.
inline RunConfig resolve_command(const RunConfig &policy, const ExecutionRequest &request) {
    auto command = policy;
    command.argv = request.argv();
    command.cwd = request.cwd() ? request.cwd()->string()
                                : (policy.mode == "isolated" ? "/output" : policy.cwd);
    if (request.env())
        command.env = *request.env();
    if (request.timeout()) {
        command.limits.wall_ms = static_cast<std::uint64_t>(request.timeout()->count());
        if (command.limits.wall_ms > policy.limits.wall_ms)
            throw SessionError("POLICY_DENIED", "command budget exceeds configured wall_ms");
    }
    command.validate(policy.mode == "process");
    if (command.to_json().dump().size() > 12000)
        throw SessionError("INVALID_REQUEST", "effective command exceeds protocol budget");
    return command;
}

// Compatibility adapter for the JSON front door and its tests. It converts once and
// delegates, so the rules above exist in exactly one place.
inline RunConfig resolve_command(const RunConfig &policy, const ExecuteRequest &request) {
    std::optional<RequestKey> key;
    if (request.request_id)
        key = RequestKey::parse(request.request_id->str());
    return resolve_command(policy, ExecutionRequest::create(request.argv, request.cwd, request.env,
                                                           request.timeout, std::move(key)));
}

inline RunConfig resolve_command(const RunConfig &policy, const Json &request) {
    return resolve_command(policy, ExecuteRequest::parse(request));
}
} // namespace sandbox
