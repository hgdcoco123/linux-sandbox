#include "sandbox/session_wire.hpp"
#include <cassert>
#include <iostream>
using namespace sandbox;

int main() {
    Json execute = {{"protocol_version",2},{"op","execute"},{"request_id","r1"},
                    {"argv",Json::array({"/bin/true"})},{"env",Json::object()},{"cwd",""},{"wall_ms",1}};
    auto decoded = session_wire::decode_request(session_wire::encode_request(execute));
    assert(decoded == execute);
    auto typed = session_wire::decode_execute(session_wire::encode_request(execute));
    assert(typed && typed->same_intent(ExecuteRequest::parse(validate_session_request(execute))));
    assert(typed->env && typed->env->empty());
    execute.erase("env"); execute.erase("cwd"); execute.erase("wall_ms");
    assert(session_wire::decode_request(session_wire::encode_request(execute)) == execute);
    execute["wall_ms"] = 1.5;
    try { session_wire::encode_request(execute); assert(false); } catch (const SessionError &) {}
    Json input = {{"protocol_version",2},{"op","stdin"},{"command_id","cmd-1"},{"bytes",Json::array({0,255})}};
    assert(session_wire::decode_request(session_wire::encode_request(input)) == input);
    assert(!session_wire::decode_execute(session_wire::encode_request(input)));
    auto stdin_request = session_wire::decode_stdin(session_wire::encode_request(input));
    assert(stdin_request && stdin_request->command_id.str() == "cmd-1");
    assert(stdin_request->bytes.size() == 2 && static_cast<unsigned char>(stdin_request->bytes[1]) == 255);
    Json request = {{"op","result"}};
    Json result = {{"command_id","cmd-1"},{"state","interrupted"},{"exit_code",nullptr},
                   {"signal",0},{"metrics",{{"counter",std::uint64_t(-1)},{"missing",nullptr}}}};
    assert(session_wire::decode_response(session_wire::encode_response(request,result)) == result);
    try { session_wire::decode_request("SBP1\xff"); assert(false); }
    catch (const SessionError &) {}
    std::cout << "session wire conversion passed\n";
}
