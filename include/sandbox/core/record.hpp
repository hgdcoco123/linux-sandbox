#pragma once
// Durable records.
//
// These are storage types, not protocol types: the accepted record is written
// before a command is dispatched, the finished record only after the result is
// final. Both carry typed business values; the JSON form is produced by
// record_json.hpp at the storage boundary and nowhere else.
#include "sandbox/core/ids.hpp"
#include "sandbox/core/request.hpp"
#include "sandbox/core/result.hpp"

#include <chrono>

namespace sandbox {

// Durable acceptance. After this record exists, the request must never be
// dispatched a second time; a retry is answered from the finished record or
// reported as indeterminate.
struct AcceptedCommand {
    CommandIndex index;
    ExecutionRequest request;
    std::chrono::system_clock::time_point accepted_at;
};

// Durable completion. Absent for a request that was accepted but whose result
// could not be confirmed - recovery must surface that as indeterminate instead
// of re-running it.
struct FinishedCommand {
    CommandIndex index;
    ExecutionResult result;
    std::chrono::system_clock::time_point finished_at;
};

} // namespace sandbox
