#pragma once
#include "sandbox/report.hpp"
#include "sandbox/session_protocol.hpp"
#include <chrono>
namespace sandbox {
class RequestId {
    std::string value_;
    explicit RequestId(std::string value) : value_(std::move(value)) {}

  public:
    static RequestId parse(std::string value) {
        validate_request_id(value);
        return RequestId(std::move(value));
    }
    const std::string &str() const {
        return value_;
    }
    bool operator==(const RequestId &) const = default;
};
class CommandId {
    std::string value_;
    explicit CommandId(std::string value) : value_(std::move(value)) {}

  public:
    static CommandId parse(std::string value) {
        validate_command_id(value);
        return CommandId(std::move(value));
    }
    const std::string &str() const {
        return value_;
    }
    bool operator==(const CommandId &) const = default;
};
struct WriteStdinRequest {
    CommandId command_id;
    std::string bytes;
};
class ExecuteRequest {
    ExecuteRequest() = default;

  public:
    std::optional<RequestId> request_id;
    std::vector<std::string> argv;
    std::optional<fs::path> cwd;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::chrono::milliseconds> timeout;
    static ExecuteRequest parse(const Json &);
    static ExecuteRequest from_fields(std::optional<std::string> request_id,
                                      std::vector<std::string> argv,
                                      std::optional<std::string> cwd,
                                      std::optional<std::map<std::string, std::string>> env,
                                      std::optional<std::uint64_t> wall_ms);
    Json json() const;
    // Request identity preserves presence and the original cwd spelling.
    bool same_intent(const ExecuteRequest &) const;
};
} // namespace sandbox
