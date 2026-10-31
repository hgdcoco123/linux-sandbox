#include "sandbox/report.hpp"
#include <stdexcept>
namespace sandbox {
std::string state_name(RunState s) {
    switch (s) {
    case RunState::created:
        return "created";
    case RunState::preparing:
        return "preparing";
    case RunState::starting:
        return "starting";
    case RunState::running:
        return "running";
    case RunState::stopping:
        return "stopping";
    case RunState::finalizing:
        return "finalizing";
    case RunState::finished:
        return "finished";
    }
    throw std::logic_error("invalid state");
}
void StateMachine::transition(RunState next) {
    bool ok = (state_ == RunState::created && next == RunState::preparing) ||
              (state_ == RunState::preparing && next == RunState::starting) ||
              (state_ == RunState::starting && next == RunState::running) ||
              ((state_ == RunState::preparing || state_ == RunState::starting ||
                state_ == RunState::running) &&
               next == RunState::stopping) ||
              (state_ == RunState::stopping && next == RunState::finalizing) ||
              (state_ == RunState::finalizing && next == RunState::finished);
    if (!ok)
        throw std::logic_error("illegal transition " + state_name(state_) + " -> " +
                               state_name(next));
    state_ = next;
}
Json StreamReport::json() const {
    return {{"saved_bytes", saved},
            {"discarded_bytes", discarded},
            {"eof", eof},
            {"flushed", flushed},
            {"queue_peak_bytes", queue_peak_bytes},
            {"backpressure_events", backpressure_events},
            {"failed_bytes", failed_bytes},
            {"write_error", write_error}};
}
Json RunReport::json() const {
    return {{"schema_version", 1},
            {"run_id", id},
            {"summary", summary},
            {"isolated", isolated},
            {"runtime_prepared", runtime_prepared},
            {"stop_reason", stop_reason},
            {"exec_observed", exec_observed},
            {"exec_error", exec_error},
            {"exit_code", exit_code ? Json(*exit_code) : Json(nullptr)},
            {"signal", term_signal ? Json(*term_signal) : Json(nullptr)},
            {"timing",
             {{"startup_ms", startup_ms}, {"execution_ms", execution_ms}, {"total_ms", total_ms}}},
            {"stdout", out.json()},
            {"stderr", err.json()},
            {"metrics", metrics},
            {"errors", errors},
            {"cleanup",
             {{"descendants_empty", descendants_empty},
              {"launcher_reaped", launcher_reaped},
              {"cgroup_removed", cgroup_removed},
              {"errors", cleanup_errors}}}};
}
void RunReport::conclude() {
    // Cleanup failures live in their own field: a target that exited 0 keeps
    // exit code 0, and callers that care about reclamation read `cleanup`.
    if (!errors.empty())
        summary = "host_error";
    else if (!exec_error.empty())
        summary = "startup_failure";
    else if (stop_reason == "oom")
        summary = "oom";
    else if (stop_reason == "timeout" || stop_reason == "cancelled" ||
             stop_reason == "startup_timeout")
        summary = stop_reason;
    else if (term_signal)
        summary = "signaled";
    else if (exit_code)
        summary = (*exit_code == 0 ? "success" : "nonzero_exit");
    else
        summary = "unknown_exit";
}
int RunReport::cli_code() const {
    if (summary == "success")
        return 0;
    if (summary == "nonzero_exit")
        return 10;
    if (summary == "signaled")
        return 11;
    if (summary == "startup_failure" || summary == "startup_timeout")
        return 12;
    if (summary == "timeout")
        return 124;
    if (summary == "cancelled")
        return 130;
    if (summary == "oom")
        return 13;
    return 125;
}
} // namespace sandbox
