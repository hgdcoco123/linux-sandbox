#pragma once
// Binary codec for the local control socket.
//
// Design notes:
//  * The wire form is an implementation detail. Callers hand over typed values
//    and get typed values back; no protobuf or JSON type appears in any
//    signature.
//  * Framing is length-prefixed with a magic and a kind byte, so a truncated or
//    mis-directed packet is rejected before any field is interpreted.
//  * The larger `ExecutionResult` payload is carried as the storage record
//    blob produced by record_json.hpp: the control layer does not reinterpret
//    it, and there is exactly one definition of that encoding.
#include "sandbox/core/ops.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace sandbox {

class WireError : public std::runtime_error {
  public:
    WireError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};

inline constexpr std::size_t max_wire_packet = 16384;
inline constexpr std::string_view wire_magic = "SBX2";

[[nodiscard]] std::string encode(const SessionRequest &);
[[nodiscard]] std::string encode(const SessionReply &);

// Throws WireError on framing, version, tag or field problems.
[[nodiscard]] SessionRequest decode_request(std::string_view packet);
[[nodiscard]] SessionReply decode_reply(std::string_view packet);

} // namespace sandbox
