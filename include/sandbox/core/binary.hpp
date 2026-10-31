#pragma once
// Minimal binary primitives shared by the control channels.
//
// Both codecs frame length-prefixed packets, so the encoding helpers live here
// instead of being copied per channel. Error handling is a policy: each codec
// supplies a type with a static fail() so a bad packet raises that channel's own
// exception with its own code.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sandbox::binary {

class Writer {
  public:
    std::string data;

    void u8(std::uint8_t value) { data.push_back(static_cast<char>(value)); }

    void varint(std::uint64_t value) {
        while (value >= 0x80) {
            data.push_back(static_cast<char>((value & 0x7f) | 0x80));
            value >>= 7;
        }
        data.push_back(static_cast<char>(value));
    }

    void text(std::string_view value) {
        varint(value.size());
        data.append(value);
    }

    void boolean(bool value) { u8(value ? 1 : 0); }

    template <class T, class F> void optional(const std::optional<T> &value, F &&body) {
        if (!value) {
            u8(0);
            return;
        }
        u8(1);
        body(*value);
    }
};

// Errors::fail(code, message) must not return.
template <class Errors> class Reader {
  public:
    explicit Reader(std::string_view input) : input_(input) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - position_; }

    std::uint8_t u8() {
        if (position_ >= input_.size())
            Errors::fail("BAD_PACKET", "truncated packet");
        return static_cast<std::uint8_t>(input_[position_++]);
    }

    std::uint64_t varint() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            auto byte = u8();
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0)
                return value;
        }
        Errors::fail("BAD_PACKET", "varint overflow");
    }

    std::string text() {
        auto length = varint();
        if (length > remaining())
            Errors::fail("BAD_PACKET", "text length exceeds packet");
        std::string value(input_.substr(position_, static_cast<std::size_t>(length)));
        position_ += static_cast<std::size_t>(length);
        return value;
    }

    bool boolean() {
        auto value = u8();
        if (value > 1)
            Errors::fail("BAD_PACKET", "invalid boolean");
        return value == 1;
    }

    template <class T, class F> std::optional<T> optional(F &&body) {
        if (!boolean())
            return std::nullopt;
        return body();
    }

    void require_end() const {
        if (position_ != input_.size())
            Errors::fail("BAD_PACKET", "trailing bytes after fields");
    }

  private:
    std::string_view input_;
    std::size_t position_ = 0;
};

} // namespace sandbox::binary
