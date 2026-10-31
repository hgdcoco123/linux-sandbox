#pragma once
// What a caller asks the sandbox to execute. Validated once, at construction.
#include "sandbox/core/ids.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sandbox {

class RequestError : public std::invalid_argument {
  public:
    RequestError(std::string code, std::string message)
        : std::invalid_argument(std::move(message)), code(std::move(code)) {}
    std::string code;
};

class ExecutionRequest {
  public:
    static constexpr std::chrono::milliseconds max_timeout{24LL * 60 * 60 * 1000};

    // Throws RequestError on a malformed request, so every precondition the
    // execution core relies on is established exactly once, here.
    //   env     nullopt = inherit the configured environment, present = use
    //           this environment verbatim (an empty map clears it).
    [[nodiscard]] static ExecutionRequest
    create(std::vector<std::string> argv, std::optional<std::filesystem::path> cwd = std::nullopt,
           std::optional<std::map<std::string, std::string>> env = std::nullopt,
           std::optional<std::chrono::milliseconds> timeout = std::nullopt,
           std::optional<RequestKey> key = std::nullopt);

    [[nodiscard]] const std::vector<std::string> &argv() const noexcept { return argv_; }
    [[nodiscard]] const std::optional<std::filesystem::path> &cwd() const noexcept { return cwd_; }
    [[nodiscard]] const std::optional<std::map<std::string, std::string>> &env() const noexcept {
        return env_;
    }
    [[nodiscard]] const std::optional<std::chrono::milliseconds> &timeout() const noexcept {
        return timeout_;
    }
    [[nodiscard]] const std::optional<RequestKey> &key() const noexcept { return key_; }

    // Two requests carry the same intent when replaying one must be answered
    // with the other's result. Presence of cwd/env is part of the intent: an
    // explicit empty env is not the same request as an inherited env.
    [[nodiscard]] bool same_intent(const ExecutionRequest &) const noexcept;

  private:
    ExecutionRequest() = default;
    std::vector<std::string> argv_;
    std::optional<std::filesystem::path> cwd_;
    std::optional<std::map<std::string, std::string>> env_;
    std::optional<std::chrono::milliseconds> timeout_;
    std::optional<RequestKey> key_;
};

} // namespace sandbox
