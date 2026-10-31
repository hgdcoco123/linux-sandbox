#pragma once
#include "sandbox/session_protocol.hpp"
#include "sandbox/command_model.hpp"
#include <string_view>
namespace sandbox::session_wire {
inline constexpr std::string_view magic = "SBP1";
Json decode_request(std::string_view packet);
std::optional<ExecuteRequest> decode_execute(std::string_view packet);
std::optional<WriteStdinRequest> decode_stdin(std::string_view packet);
std::string encode_request(const Json &request);
std::string encode_response(const Json &request, const Json &answer);
std::string encode_error(const SessionError &error);
Json decode_response(std::string_view packet);
}
