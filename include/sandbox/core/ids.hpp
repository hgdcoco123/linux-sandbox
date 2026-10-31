#pragma once
// Typed identifiers.
//
// The point of these types is that a constructed identifier is always valid:
// the only ways to obtain one are parse() (validates external text) and mint()
// (generates it from trusted inputs). Nothing downstream re-validates, and no
// caller can hand a half-checked string to the execution core.
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sandbox {

enum class IdError { empty, too_long, bad_character, bad_prefix, bad_digits, out_of_range };

const char *id_error_name(IdError) noexcept;

// ExecutionId: "run-" + 24 lowercase hex digits, minted from 12 random bytes.
class ExecutionId {
  public:
    static constexpr std::string_view prefix = "run-";
    static constexpr std::size_t hex_digits = 24;

    [[nodiscard]] static std::optional<ExecutionId> parse(std::string_view text) noexcept;
    [[nodiscard]] static ExecutionId mint(const unsigned char (&random)[12]);

    [[nodiscard]] const std::string &str() const noexcept { return value_; }
    friend bool operator==(const ExecutionId &, const ExecutionId &) = default;

  private:
    explicit ExecutionId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

// RequestKey: caller-supplied idempotency key. 1..64 of [A-Za-z0-9_-].
class RequestKey {
  public:
    static constexpr std::size_t max_length = 64;

    [[nodiscard]] static std::optional<RequestKey> parse(std::string_view text) noexcept;
    [[nodiscard]] const std::string &str() const noexcept { return value_; }
    friend bool operator==(const RequestKey &, const RequestKey &) = default;

  private:
    explicit RequestKey(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

// CommandIndex: position of a command inside a session. 1-based, so 0 can mean
// "no command" and the textual form stays "cmd-<n>".
class CommandIndex {
  public:
    static constexpr std::uint32_t first = 1;
    [[nodiscard]] static std::optional<CommandIndex> from_number(std::uint32_t n) noexcept;
    [[nodiscard]] static std::optional<CommandIndex> parse(std::string_view text) noexcept;

    [[nodiscard]] std::uint32_t number() const noexcept { return number_; }
    [[nodiscard]] std::string str() const { return "cmd-" + std::to_string(number_); }
    friend auto operator<=>(const CommandIndex &, const CommandIndex &) = default;

  private:
    explicit CommandIndex(std::uint32_t n) : number_(n) {}
    std::uint32_t number_;
};

} // namespace sandbox
