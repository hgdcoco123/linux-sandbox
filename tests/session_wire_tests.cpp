#include "sandbox/session_wire.hpp"
#include <cassert>
#include <iostream>
using namespace sandbox;

int main() {
    Json execute = {{"protocol_version",2},{"op","execute"},{"request_id","r1"},
                    {"argv",Json::array({"/bin/true"})},{"env",Json::object()},
                    {"cwd","/tmp"},{"wall_ms",1000}};
    auto decoded = session_wire::decode_request(session_wire::encode_request(execute));
    assert(decoded == execute);
    // The protobuf path lands on the same business value, so there is one decoder
    // and one set of rules behind two front doors.
    auto typed = session_wire::decode_session_request(session_wire::encode_request(execute));
    assert(typed.application_version == 2);
    const auto *typed_execute = typed.request.as<Execute>();
    assert(typed_execute && typed_execute->request.key() &&
           typed_execute->request.key()->str() == "r1");
    assert(typed_execute->request.env() && typed_execute->request.env()->empty());
    assert(typed_execute->request.cwd() && *typed_execute->request.cwd() == "/tmp");
    // A request the JSON door rejects is rejected here too: the type is the rule.
    auto invalid = execute;
    invalid["cwd"] = "";
    try {
        session_wire::decode_session_request(session_wire::encode_request(invalid));
        assert(false);
    } catch (const SessionError &failure) {
        assert(failure.code == "INVALID_REQUEST");
    }
    auto zero_wall = execute;
    zero_wall["wall_ms"] = 0;
    try {
        session_wire::decode_session_request(session_wire::encode_request(zero_wall));
        assert(false);
    } catch (const SessionError &) {
    }
    execute.erase("env"); execute.erase("cwd"); execute.erase("wall_ms");
    assert(session_wire::decode_request(session_wire::encode_request(execute)) == execute);
    execute["wall_ms"] = 1.5;
    try { session_wire::encode_request(execute); assert(false); } catch (const SessionError &) {}
    Json input = {{"protocol_version",2},{"op","stdin"},{"command_id","cmd-1"},{"bytes",Json::array({0,255})}};
    assert(session_wire::decode_request(session_wire::encode_request(input)) == input);
    auto stdin_request = session_wire::decode_session_request(session_wire::encode_request(input));
    const auto *write = stdin_request.request.as<StdinWrite>();
    assert(write && write->command.str() == "cmd-1");
    assert(write->bytes.size() == 2 && static_cast<unsigned char>(write->bytes[1]) == 255);
    // A malformed command id is refused at the door, not inside the controller.
    Json bad_command = {{"protocol_version",2},{"op","result"},{"command_id","cmd-01"}};
    try {
        session_wire::decode_session_request(session_wire::encode_request(bad_command));
        assert(false);
    } catch (const SessionError &failure) {
        assert(failure.code == "INVALID_REQUEST");
    }
    Json result = {{"command_id","cmd-1"},{"state","interrupted"},{"exit_code",nullptr},
                   {"signal",0},{"metrics",{{"counter",std::uint64_t(-1)},{"missing",nullptr}}}};
    assert(session_wire::decode_response(session_wire::encode_response("result", result)) == result);
    try { session_wire::decode_session_request("SBP1\xff"); assert(false); }
    catch (const SessionError &) {}
    std::cout << "session wire conversion passed\n";
}
