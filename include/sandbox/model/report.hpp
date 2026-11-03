#pragma once
#include "sandbox/model/config.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/core/result.hpp"
#include <optional>
namespace sandbox {
enum class RunState { created, preparing, starting, running, stopping, finalizing, finished };
std::string state_name(RunState);
class StateMachine {
  public:
    void transition(RunState next);
    RunState current() const {
        return state_;
    }

  private:
    RunState state_ = RunState::created;
};
struct StreamReport {
    std::uint64_t saved = 0, discarded = 0;
    bool eof = false, flushed = false;
    std::uint64_t queue_peak_bytes = 0, backpressure_events = 0, failed_bytes = 0;
    std::string write_error;
    Json json() const;
};
// One conversion, one place: both the single-run and the session controller feed the
// execution core the same OutputStats, and a second copy is a second thing to drift.
inline OutputStats stats_of(const StreamReport &report) {
    OutputStats stats;
    stats.saved_bytes = report.saved;
    stats.discarded_bytes = report.discarded;
    stats.failed_bytes = report.failed_bytes;
    stats.queue_peak_bytes = report.queue_peak_bytes;
    stats.backpressure_events = report.backpressure_events;
    stats.write_error = report.write_error;
    return stats;
}

struct RunReport {
    std::string id, summary = "host_error", stop_reason, exec_error;
    // One actionable word for the whole run (ok / target_failed / harness_error /
    // cleanup_incomplete). `summary` keeps naming the target result; this is the
    // verdict a caller should branch on.
    std::string verdict = "unknown";
    bool isolated = false, exec_observed = false, descendants_empty = false;
    // Which syscall policy the target ran under, copied from the execution result.
    SecurityPolicyFacts security;
    bool launcher_reaped = false, cgroup_removed = false;
    bool runtime_prepared = false;
    std::optional<int> exit_code, term_signal;
    std::uint64_t startup_ms = 0, execution_ms = 0, total_ms = 0;
    StreamReport out, err;
    Json metrics = Json::object();
    // Facts about the sandbox-owned workspace: how it is bounded, and whether the
    // artifacts actually reached the host. Null when the run has no workspace to
    // deliver (process mode, or isolated without an /output destination).
    Json workspace = Json(nullptr);
    std::vector<std::string> errors, cleanup_errors;
    Json json() const;
    void conclude();
    int cli_code() const;
};
} // namespace sandbox
