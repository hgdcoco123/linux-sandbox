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

inline constexpr int record_schema_version = 2;

Json encode(const AcceptedCommand &);
Json encode(const FinishedCommand &);

// Returns nullopt and fills `error` when the record is absent or malformed.
std::optional<AcceptedCommand> decode_accepted(const Json &, std::string &error);
std::optional<FinishedCommand> decode_finished(const Json &, std::string &error);

} // namespace sandbox
