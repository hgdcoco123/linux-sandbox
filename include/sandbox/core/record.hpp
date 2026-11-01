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
#include <filesystem>
#include <optional>

namespace sandbox {

// Durable acceptance. After this record exists, the request must never be
// dispatched a second time; a retry is answered from the finished record or
// reported as indeterminate.
struct AcceptedCommand {
    CommandIndex index;
    ExecutionRequest request;
    std::chrono::system_clock::time_point accepted_at;
    // Sessions resolve the working directory once, before dispatch: a caller may
    // omit cwd and the session default applies. That resolved path is a fact
    // about the command, but it is not part of the request's intent (replay
    // compares the request exactly as it arrived, presence of cwd included), so
    // it is stored beside the request rather than folded into it. Absent for a
    // single run, where the request is the whole story.
    std::optional<std::filesystem::path> effective_cwd;
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
