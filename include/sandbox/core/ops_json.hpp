#pragma once
// JSON -> typed session request adapter.
//
// The socket layer still speaks JSON (the Python SDK depends on it), so the
// conversion lives here and nowhere else: the business layer below this point
// only ever sees SessionRequest. Decoding never throws; a malformed request is
// data to report, not an exception to unwind.
#include "sandbox/core/ops.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace sandbox {

// Returns nullopt and fills `error` when the request is malformed. The error
// text names the offending field, because it reaches the caller verbatim.
[[nodiscard]] std::optional<SessionRequest> session_request_from_json(const nlohmann::json &,
                                                                     std::string &error);

} // namespace sandbox
