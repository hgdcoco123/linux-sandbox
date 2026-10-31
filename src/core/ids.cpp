#include "sandbox/core/ids.hpp"

#include <charconv>

namespace sandbox {
namespace {
bool is_lower_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
bool is_key_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
           c == '_';
}
} // namespace

const char *id_error_name(IdError error) noexcept {
    switch (error) {
    case IdError::empty:
        return "empty";
    case IdError::too_long:
        return "too_long";
    case IdError::bad_character:
        return "bad_character";
    case IdError::bad_prefix:
        return "bad_prefix";
    case IdError::bad_digits:
        return "bad_digits";
    case IdError::out_of_range:
        return "out_of_range";
    }
    return "unknown";
}

std::optional<ExecutionId> ExecutionId::parse(std::string_view text) noexcept {
    if (text.size() != prefix.size() + hex_digits)
        return std::nullopt;
    if (text.substr(0, prefix.size()) != prefix)
        return std::nullopt;
    for (char c : text.substr(prefix.size()))
        if (!is_lower_hex(c))
            return std::nullopt;
    return ExecutionId(std::string(text));
}

ExecutionId ExecutionId::mint(const unsigned char (&random)[12]) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value(prefix);
    value.reserve(prefix.size() + hex_digits);
    for (std::size_t i = 0; i < 12; ++i) {
        value.push_back(digits[random[i] >> 4]);
        value.push_back(digits[random[i] & 0x0f]);
    }
    return ExecutionId(std::move(value));
}

std::optional<RequestKey> RequestKey::parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > max_length)
        return std::nullopt;
    for (char c : text)
        if (!is_key_char(c))
            return std::nullopt;
    return RequestKey(std::string(text));
}

std::optional<CommandIndex> CommandIndex::from_number(std::uint32_t number) noexcept {
    if (number < first || number > 1'000'000)
        return std::nullopt;
    return CommandIndex(number);
}

std::optional<CommandIndex> CommandIndex::parse(std::string_view text) noexcept {
    constexpr std::string_view marker = "cmd-";
    if (text.size() <= marker.size() || text.substr(0, marker.size()) != marker)
        return std::nullopt;
    auto digits = text.substr(marker.size());
    if (digits.size() > 1 && digits.front() == '0')
        return std::nullopt; // no leading zeroes: one spelling per index
    std::uint32_t number = 0;
    auto result = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
        return std::nullopt;
    return from_number(number);
}

} // namespace sandbox
