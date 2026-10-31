#pragma once
#include "sandbox/command_model.hpp"
namespace sandbox {
inline RunConfig resolve_command(const RunConfig &policy, const ExecuteRequest &request) {
    auto command = policy;
    command.argv = request.argv;
    command.cwd =
        request.cwd ? request.cwd->string() : (policy.mode == "isolated" ? "/output" : policy.cwd);
    if (request.env)
        command.env = *request.env;
    if (request.timeout) {
        command.limits.wall_ms = request.timeout->count();
        if (command.limits.wall_ms > policy.limits.wall_ms)
            throw SessionError("POLICY_DENIED", "command budget exceeds configured wall_ms");
    }
    command.validate(policy.mode == "process");
    if (command.to_json().dump().size() > 12000)
        throw SessionError("INVALID_REQUEST", "effective command exceeds protocol budget");
    return command;
}
// Compatibility parser boundary for older callers/tests; core uses the typed overload.
inline RunConfig resolve_command(const RunConfig &policy, const Json &request) {
    return resolve_command(policy, ExecuteRequest::parse(request));
}
} // namespace sandbox
