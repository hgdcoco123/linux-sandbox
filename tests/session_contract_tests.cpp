#include "sandbox/session_policy.hpp"
#include "sandbox/session_state.hpp"
#include <iostream>
using namespace sandbox;
void require(bool value) {
    if (!value)
        throw std::runtime_error("contract assertion");
}
template <class F> void rejects(F f, const std::string &code = "") {
    try {
        f();
    } catch (const SessionError &e) {
        require(code.empty() || code == e.code);
        return;
    } catch (const std::exception &) {
        require(code.empty());
        return;
    }
    throw std::runtime_error("expected rejection");
}
int main() {
    try {
        SessionLifecycle state;
        rejects([&] { state.require_idle(); }, "SESSION_NOT_IDLE");
        state.transition(SessionState::idle);
        state.require_idle();
        state.transition(SessionState::running);
        state.require_running();
        rejects([&] { state.transition(SessionState::idle); });
        state.transition(SessionState::draining);
        rejects([&] { state.require_idle(); }, "SESSION_NOT_IDLE");
        rejects([&] { state.require_running(); }, "COMMAND_NOT_ACTIVE");
        state.transition(SessionState::idle);
        state.transition(SessionState::closing);
        rejects([&] { state.require_idle(); }, "SESSION_CLOSING");
        rejects([&] { state.transition(SessionState::idle); });
        state.transition(SessionState::closed);
        rejects([&] { state.transition(SessionState::running); });
        SessionLifecycle early;
        early.transition(SessionState::closing);
        rejects([&] { early.transition(SessionState::idle); });
        early.transition(SessionState::failed);
        early.transition(SessionState::closed);
        Json request = {{"protocol_version", 2},
                        {"op", "execute"},
                        {"argv", {"/bin/true"}},
                        {"request_id", "retry-1"}};
        auto validated = validate_session_request(request);
        require(!validated.contains("protocol_version"));
        auto identity = ExecuteRequest::parse(validated);
        require(identity.same_intent(ExecuteRequest::parse(identity.json())));
        auto changed = validated;
        changed["env"] = Json::object();
        require(!identity.same_intent(ExecuteRequest::parse(changed)));
        changed = validated;
        changed["cwd"] = "/tmp//project";
        auto doubled = ExecuteRequest::parse(changed);
        changed["cwd"] = "/tmp/project";
        require(!doubled.same_intent(ExecuteRequest::parse(changed)));
        auto missing = request;
        missing.erase("request_id");
        rejects([&] { validate_session_request(missing); }, "INVALID_REQUEST");
        missing.erase("protocol_version");
        validate_session_request(missing); // explicit legacy support
        request["protocol_version"] = 9;
        rejects([&] { validate_session_request(request); }, "UNSUPPORTED_VERSION");
        rejects([&] { validate_request_id("../escape"); }, "INVALID_REQUEST");
        RunConfig policy;
        policy.mode = "process";
        policy.network = "host";
        policy.argv = {"/bin/true"};
        auto resolved = resolve_command(policy, validated);
        require(resolved.limits.memory_bytes == policy.limits.memory_bytes);
        validated["wall_ms"] = policy.limits.wall_ms + 1;
        rejects([&] { resolve_command(policy, validated); }, "POLICY_DENIED");
        validated["wall_ms"] = -1;
        rejects([&] { resolve_command(policy, validated); }, "INVALID_REQUEST");
        validated.erase("wall_ms");
        validated["mounts"] = Json::array();
        rejects([&] { resolve_command(policy, validated); });
        std::cout << "session lifecycle, protocol and policy contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
