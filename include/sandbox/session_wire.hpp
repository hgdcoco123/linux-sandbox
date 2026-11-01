#pragma once
#include "sandbox/core/ops.hpp"
#include "sandbox/session_protocol.hpp"

#include <cstdint>
#include <string_view>

namespace sandbox::session_wire {
inline constexpr std::string_view magic = "SBP1";

// A decoded request plus the envelope version it arrived with. The version is
// wire metadata, not a business fact, so it stays out of SessionRequest.
struct DecodedRequest {
    SessionRequest request;
    std::uint32_t application_version = 1;
};

// Protobuf -> business request, decoded once. There is deliberately no JSON step:
// a decoder that produces JSON only to have it decoded again is a second
// implementation of the same validation, and the two drift apart silently.
//
// Throws SessionError for a malformed packet, exactly like the JSON front door,
// so both entry points report the same codes.
[[nodiscard]] DecodedRequest decode_session_request(std::string_view packet);

// The legacy JSON spelling of a typed request, for the CLI and the interop
// bridge, which still speak JSON. This is a rendering, not a second decoder.
[[nodiscard]] Json session_request_to_json(const SessionRequest &, std::uint32_t application_version);

// The operation's wire name ("execute", "stdin", "request-status", ...).
[[nodiscard]] std::string_view operation_name(const SessionOperation &) noexcept;

// A durable command report in the wire's command vocabulary.
//
// This is where the record meets the protocol: storage returns the two records and
// has no opinion about how a reply spells them. A report without a finished record
// says "uncertain" - it was accepted and no outcome has been observed.
[[nodiscard]] Json command_report_to_json(const CommandReport &);

// Rendering of a decoded packet, used by the JSON front door. Kept so the CLI and
// the SDK never need to know protobuf.
[[nodiscard]] Json decode_request(std::string_view packet);

// Client side: JSON request -> protobuf, validated before conversion.
std::string encode_request(const Json &request);
std::string encode_response(std::string_view operation, const Json &answer);
std::string encode_error(const SessionError &error);
[[nodiscard]] Json decode_response(std::string_view packet);
} // namespace sandbox::session_wire
