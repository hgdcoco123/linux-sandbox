#pragma once
#include "sandbox/model/config.hpp"
#include <stdexcept>
namespace sandbox {
inline constexpr unsigned session_protocol_version = 2;
inline constexpr std::size_t session_message_limit = 16384;
class SessionError : public std::runtime_error {
  public:
    std::string code;
    bool retryable;
    SessionError(std::string code, std::string message, bool retryable = false)
        : std::runtime_error(std::move(message)), code(std::move(code)), retryable(retryable) {}
    Json json() const {
        return {{"code", code}, {"message", what()}, {"retryable", retryable}};
    }
};
inline void validate_request_id(const std::string &id) {
    if (id.empty() || id.size() > 64 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
        throw SessionError("INVALID_REQUEST",
                           "request_id must be 1..64 ASCII letters, digits, '-' or '_'");
}
inline void validate_command_id(const std::string &id) {
    if (id.size() < 5 || id.size() > 12 || id.rfind("cmd-", 0) != 0 ||
        id.find_first_not_of("0123456789", 4) != std::string::npos)
        throw SessionError("INVALID_REQUEST", "invalid command ID");
}
inline Json validate_session_request(const Json &input, bool check_encoded_size = true) {
    if (!input.is_object())
        throw SessionError("INVALID_REQUEST", "request must be an object");
    if (check_encoded_size && input.dump().size() > session_message_limit)
        throw SessionError("INVALID_REQUEST", "request exceeds 16KiB");
    auto version = input.value("protocol_version", 1);
    if (version != 1 && version != 2)
        throw SessionError("UNSUPPORTED_VERSION", "supported protocol versions: 1, 2");
    auto op = input.at("op").get<std::string>();
    if (input.contains("request_id"))
        validate_request_id(input.at("request_id").get<std::string>());
    if (version == 2 && op == "execute" && !input.contains("request_id"))
        throw SessionError("INVALID_REQUEST", "protocol v2 execute requires request_id");
    auto j = input;
    j.erase("protocol_version");
    if (op == "execute")
        require_keys(j, {"op", "request_id", "argv", "cwd", "env", "wall_ms"}, op);
    else if (op == "status" || op == "close")
        require_keys(j, {"op"}, op);
    else if (op == "cancel" || op == "stdin-close")
        require_keys(j, {"op", "command_id"}, op);
    else if (op == "stdin")
        require_keys(j, {"op", "command_id", "bytes"}, op);
    else if (op == "output")
        require_keys(j, {"op", "command_id", "stream", "offset"}, op);
    else if (op == "result")
        require_keys(j, {"op", "command_id"}, op);
    else if (op == "request-status")
        require_keys(j, {"op", "request_id"}, op);
    else if (op == "files")
        require_keys(j, {"op", "request"}, op);
    else
        throw SessionError("UNKNOWN_OPERATION", "unknown session operation");
    return j;
}
} // namespace sandbox
