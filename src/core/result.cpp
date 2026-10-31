#include "sandbox/core/result.hpp"

namespace sandbox {

const char *cleanup_stage_name(CleanupStage stage) noexcept {
    switch (stage) {
    case CleanupStage::descendants:
        return "descendants";
    case CleanupStage::launcher:
        return "launcher";
    case CleanupStage::init:
        return "init";
    case CleanupStage::cgroup:
        return "cgroup";
    case CleanupStage::mounts:
        return "mounts";
    case CleanupStage::outputs:
        return "outputs";
    case CleanupStage::workspace:
        return "workspace";
    case CleanupStage::control_socket:
        return "control_socket";
    }
    return "unknown";
}

int outcome_exit_code(const ExecutionOutcome &outcome) noexcept {
    if (auto code = outcome.exit_code())
        return *code == 0 ? 0 : 10;
    if (outcome.term_signal())
        return 11;
    if (std::holds_alternative<TimedOut>(outcome.variant()))
        return 124;
    if (std::holds_alternative<Cancelled>(outcome.variant()))
        return 130;
    if (std::holds_alternative<Oom>(outcome.variant()))
        return 13;
    if (std::holds_alternative<StartupFailed>(outcome.variant()))
        return 12;
    // SupervisionFailed and Indeterminate: the harness could not produce a
    // trustworthy target result.
    return 125;
}

Verdict verdict_of(const ExecutionResult &result) noexcept {
    if (std::holds_alternative<SupervisionFailed>(result.outcome.variant()) ||
        std::holds_alternative<Indeterminate>(result.outcome.variant()))
        return Verdict::harness_error;
    // A target that exited 0 but left resources behind is neither a success nor
    // a target failure; it gets its own verdict. Incomplete output counts: bytes
    // that never reached the sink, or a writer that failed, mean the report
    // cannot be trusted even when the exit status can.
    if (!result.cleanup.complete() || !result.out.complete() || !result.err.complete())
        return Verdict::cleanup_incomplete;
    if (result.target_succeeded())
        return Verdict::ok;
    if (std::holds_alternative<StartupFailed>(result.outcome.variant()))
        return Verdict::harness_error;
    return Verdict::target_failed;
}

const char *verdict_name(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::ok:
        return "ok";
    case Verdict::target_failed:
        return "target_failed";
    case Verdict::harness_error:
        return "harness_error";
    case Verdict::cleanup_incomplete:
        return "cleanup_incomplete";
    }
    return "unknown";
}

} // namespace sandbox
