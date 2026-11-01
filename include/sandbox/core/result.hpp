#pragma once
// The result of one execution, split into the three independent facts a caller
// actually needs to distinguish:
//
//   outcome  - what happened to the target process
//   output   - what was captured, discarded or failed to reach disk
//   cleanup  - what of the environment was actually reclaimed
//
// They are deliberately not collapsed into one status string. "Target exited 0,
// workspace deletion failed" and "target result unknown, everything reclaimed"
// are both representable and both distinguishable.
#include "sandbox/core/outcome.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sandbox {

struct Timing {
    std::chrono::microseconds startup{0};
    std::chrono::microseconds execution{0};
    std::chrono::microseconds total{0};
    friend bool operator==(const Timing &, const Timing &) = default;
};

struct OutputResult {
    std::uint64_t saved_bytes = 0;
    std::uint64_t discarded_bytes = 0; // over the capture cap; still drained
    std::uint64_t failed_bytes = 0;    // write() to the sink failed
    std::uint64_t queue_peak_bytes = 0;
    std::uint64_t backpressure_events = 0;
    bool eof = false;      // the producing pipe reached EOF
    bool flushed = false;  // every byte accepted by the writer reached the kernel
    std::string write_error;

    [[nodiscard]] bool truncated() const noexcept { return discarded_bytes != 0; }
    [[nodiscard]] bool write_failed() const noexcept { return failed_bytes != 0 || !write_error.empty(); }
    // Complete means: producer finished, writer finished, and nothing was lost
    // other than what the capture cap intentionally discards.
    [[nodiscard]] bool complete() const noexcept { return eof && flushed && !write_failed(); }
};

enum class CleanupStage {
    descendants,
    launcher,
    init,
    cgroup,
    mounts,
    outputs,
    workspace,
    control_socket,
};

const char *cleanup_stage_name(CleanupStage) noexcept;

struct CleanupFailure {
    CleanupStage stage = CleanupStage::descendants;
    std::string detail;
    friend bool operator==(const CleanupFailure &, const CleanupFailure &) = default;
};

struct CleanupResult {
    bool descendants_gone = false;
    bool launcher_reaped = false;
    bool init_reaped = false;
    bool cgroup_removed = false;
    bool workspace_removed = false;
    std::vector<CleanupFailure> failures;

    [[nodiscard]] bool complete() const noexcept { return failures.empty() && descendants_gone; }
};

// The syscall policy the target actually ran under.
//
// Recorded instead of inferred from the configuration: a result that says
// "baseline" without a version cannot tell you which rule set produced it, and two
// results are only comparable when the version matches. An empty name means no
// filter was installed at all, which is a different fact from "the default one".
struct SecurityPolicyFacts {
    std::string name;
    std::uint32_t version = 0;

    friend bool operator==(const SecurityPolicyFacts &, const SecurityPolicyFacts &) = default;
};

struct ExecutionResult {
    ExecutionOutcome outcome;
    OutputResult out;
    OutputResult err;
    CleanupResult cleanup;
    Timing timing;
    SecurityPolicyFacts security;

    // The target's own outcome, ignoring how the environment was reclaimed.
    [[nodiscard]] bool target_succeeded() const noexcept { return outcome.is_success(); }
    // Followers of a failed shutdown must not be mistaken for a failed target.
    [[nodiscard]] bool cleanup_failed() const noexcept { return !cleanup.complete(); }
};

// Process exit status for the CLI/launcher. Derives from the outcome only:
// cleanup failures are reported through their own field so that a target that
// exited 0 is never silently rewritten into "host error".
[[nodiscard]] int outcome_exit_code(const ExecutionOutcome &) noexcept;

// Operators still need one actionable verdict for alerting.
enum class Verdict { ok, target_failed, harness_error, cleanup_incomplete };
[[nodiscard]] Verdict verdict_of(const ExecutionResult &) noexcept;
[[nodiscard]] const char *verdict_name(Verdict) noexcept;

} // namespace sandbox
