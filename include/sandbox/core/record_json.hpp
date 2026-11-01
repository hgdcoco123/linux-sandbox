#pragma once
// The only place where core business types meet JSON.
//
// Decoding never throws: a corrupted record is data to report, not an
// exception to unwind. Encoding is total.
#include "sandbox/core/record.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace sandbox {

using Json = nlohmann::json;

// Storage format version. This is the *record* format, deliberately independent
// of both the session protocol version and the control-frame version: a stored
// record that cannot be decoded is reported, never guessed at.
//
//   2 - first typed record shape (single run only)
//   3 - adds the session facts: accepted.effective_cwd, cancelled.cause,
//       startup_failure.exec_errno
inline constexpr int record_schema_version = 3;

Json encode(const AcceptedCommand &);
Json encode(const FinishedCommand &);

// Returns nullopt and fills `error` when the record is absent or malformed.
std::optional<AcceptedCommand> decode_accepted(const Json &, std::string &error);
std::optional<FinishedCommand> decode_finished(const Json &, std::string &error);

} // namespace sandbox
