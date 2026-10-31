#include "sandbox/core/supervisor.hpp"

namespace sandbox {
namespace {
constexpr int sigterm = 15;
} // namespace

const char *phase_name(Phase phase) noexcept {
    switch (phase) {
    case Phase::idle:
        return "idle";
    case Phase::starting:
        return "starting";
    case Phase::running:
        return "running";
    case Phase::stopping:
        return "stopping";
    case Phase::draining:
        return "draining";
    case Phase::finished:
        return "finished";
    case Phase::failed:
        return "failed";
    }
    return "unknown";
}

const char *stop_intent_name(StopIntent intent) noexcept {
    switch (intent) {
    case StopIntent::cancel:
        return "cancelled";
    case StopIntent::timeout:
        return "timeout";
    case StopIntent::session_closed:
        return "session_closed";
    }
    return "unknown";
}

ExecutionCore::ExecutionCore(SupervisionLimits limits, Millis created_at)
    : limits_(limits), created_at_(created_at) {
    phase_ = Phase::starting;
    result_.outcome = ExecutionOutcome(Indeterminate{"execution did not start"});
}

std::vector<SupervisionAction> ExecutionCore::env_ready(Millis now) {
    if (phase_ != Phase::starting)
        return {};
    phase_ = Phase::running;
    exec_at_ = now;
    result_.timing.startup = std::chrono::duration_cast<std::chrono::microseconds>(now - created_at_);
    return {};
}

std::vector<SupervisionAction> ExecutionCore::exec_observed(Millis now) {
    // The execution budget started at env_ready; observing exec only confirms
    // the target really ran, which the report needs for its own reasons.
    (void)now;
    return {};
}

void ExecutionCore::decide_outcome() {
    if (outcome_)
        return;
    // A fact about the target always wins over the reason we asked it to stop:
    // "exited 0" cannot be rewritten into "cancelled" afterwards.
    if (natural_exit_) {
        outcome_ = ExecutionOutcome(Exited{*natural_exit_});
    } else if (natural_signal_) {
        outcome_ = oom_ ? ExecutionOutcome(Oom{}) : ExecutionOutcome(Signaled{*natural_signal_});
    } else if (intent_ == StopIntent::timeout) {
        outcome_ = ExecutionOutcome(TimedOut{});
    } else if (intent_) {
        outcome_ = ExecutionOutcome(Cancelled{});
    } else if (oom_) {
        outcome_ = ExecutionOutcome(Oom{});
    }
}

void ExecutionCore::begin_drain(Millis now) {
    if (phase_ == Phase::draining || phase_ == Phase::finished || phase_ == Phase::failed)
        return;
    phase_ = Phase::draining;
    drain_at_ = now;
    if (exit_at_.count() == 0)
        exit_at_ = now;
    decide_outcome();
}

std::vector<SupervisionAction> ExecutionCore::main_exited(std::optional<int> exit_code,
                                                          std::optional<int> term_signal,
                                                          Millis now) {
    if (finished_)
        return {};
    if (phase_ == Phase::stopping) {
        // We already asked it to stop: record what the kernel reported, but the
        // outcome stays the stop reason.
        if (term_signal)
            stopped_signal_ = term_signal;
        exit_at_ = now;
        begin_drain(now);
        return maybe_finish(now);
    }
    if (!natural_exit_ && !natural_signal_) {
        natural_exit_ = exit_code;
        natural_signal_ = term_signal;
    }
    exit_at_ = now;
    begin_drain(now);
    // A dead main process does not mean dead descendants.
    std::vector<SupervisionAction> actions{SignalTarget{sigterm}};
    auto finished = maybe_finish(now);
    actions.insert(actions.end(), finished.begin(), finished.end());
    return actions;
}

std::vector<SupervisionAction> ExecutionCore::stop_requested(StopIntent intent, Millis now) {
    if (finished_)
        return {};
    if (intent_)
        return {}; // first reason wins
    intent_ = intent;
    stop_at_ = now;
    if (outcome_decided() || phase_ == Phase::draining) {
        // The target already exited; we are only waiting on its descendants.
        return maybe_finish(now);
    }
    if (phase_ == Phase::failed)
        return maybe_finish(now);
    phase_ = Phase::stopping;
    return {SignalTarget{sigterm}};
}

std::vector<SupervisionAction> ExecutionCore::oom_observed(Millis now) {
    oom_ = true;
    decide_outcome();
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::descendants_empty(Millis now) {
    descendants_gone_ = true;
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::output_eof(bool is_stderr, Millis now) {
    (is_stderr ? err_eof_ : out_eof_) = true;
    auto &stream = is_stderr ? result_.err : result_.out;
    stream.eof = true;
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::output_finished(bool is_stderr, const OutputStats &stats,
                                                              Millis now) {
    auto &stream = is_stderr ? result_.err : result_.out;
    stream.saved_bytes = stats.saved_bytes;
    stream.discarded_bytes = stats.discarded_bytes;
    stream.failed_bytes = stats.failed_bytes;
    stream.queue_peak_bytes = stats.queue_peak_bytes;
    stream.backpressure_events = stats.backpressure_events;
    stream.write_error = stats.write_error;
    // The writer drained everything it accepted to the kernel; durability is a
    // separate question the report never claims to answer.
    stream.flushed = true;
    (is_stderr ? err_finished_ : out_finished_) = true;
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::supervisor_failed(std::string reason, Millis now) {
    if (!outcome_ && phase_ != Phase::draining)
        outcome_ = ExecutionOutcome(SupervisionFailed{std::move(reason)});
    if (phase_ != Phase::finished)
        phase_ = Phase::failed;
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::prepare_failed(std::string reason, Millis now) {
    if (finished_)
        return {};
    // Nothing was started, so there is no output to wait for and no descendant
    // to reap. Saying so explicitly is not the same as inventing facts.
    outcome_ = ExecutionOutcome(StartupFailed{std::move(reason)});
    result_.outcome = *outcome_;
    out_finished_ = err_finished_ = true;
    out_eof_ = err_eof_ = true;
    result_.out.eof = result_.out.flushed = true;
    result_.err.eof = result_.err.flushed = true;
    descendants_gone_ = true;
    phase_ = Phase::failed;
    result_.timing.total = std::chrono::duration_cast<std::chrono::microseconds>(now - created_at_);
    result_.cleanup.descendants_gone = true;
    finished_ = true;
    phase_ = Phase::finished;
    return {Completed{}};
}

std::vector<SupervisionAction> ExecutionCore::start_failed(std::string reason, Millis now) {
    if (finished_)
        return {};
    if (!outcome_)
        outcome_ = ExecutionOutcome(StartupFailed{std::move(reason)});
    if (phase_ == Phase::stopping || phase_ == Phase::draining || phase_ == Phase::failed)
        return maybe_finish(now);
    phase_ = Phase::stopping;
    stop_at_ = now;
    return {SignalTarget{sigterm}};
}

std::vector<SupervisionAction> ExecutionCore::tick(Millis now) {
    if (finished_)
        return {};
    if (phase_ == Phase::starting) {
        if (now - created_at_ > limits_.startup) {
            if (!outcome_)
                outcome_ = ExecutionOutcome(StartupFailed{"startup timeout"});
            phase_ = Phase::stopping;
            stop_at_ = now;
            // The environment never became ready; ask the group to go away and
            // still wait for the real cleanup facts.
            return {SignalTarget{sigterm}};
        }
        return {};
    }
    if (phase_ == Phase::running && now - exec_at_ > limits_.wall)
        return stop_requested(StopIntent::timeout, now);
    if (phase_ == Phase::stopping && !killed_ && now - stop_at_ > limits_.grace) {
        killed_ = true;
        return {KillTarget{}};
    }
    if ((phase_ == Phase::stopping || phase_ == Phase::draining) && !drain_forced_ &&
        now - drain_at_ > limits_.drain) {
        drain_forced_ = true;
        std::vector<SupervisionAction> actions{ForceDrain{}};
        auto finished = maybe_finish(now);
        actions.insert(actions.end(), finished.begin(), finished.end());
        return actions;
    }
    return maybe_finish(now);
}

std::vector<SupervisionAction> ExecutionCore::maybe_finish(Millis now) {
    if (finished_)
        return {};
    const bool ready = outcome_decided() && descendants_gone_ && outputs_finished();
    const Millis anchor = exit_at_.count() != 0 ? exit_at_ : stop_at_;
    const bool expired = (phase_ == Phase::stopping || phase_ == Phase::draining) &&
                         anchor.count() != 0 && now - anchor > limits_.drain + limits_.settle;
    if (!ready && !expired)
        return {};

    if (!outcome_)
        outcome_ = ExecutionOutcome(Indeterminate{"execution ended without a confirmed result"});
    result_.outcome = *outcome_;

    if (!descendants_gone_)
        add_cleanup_failure(CleanupStage::descendants, "descendants still present at deadline");
    if (!out_finished_)
        add_cleanup_failure(CleanupStage::outputs, "stdout writer did not finish");
    if (!err_finished_)
        add_cleanup_failure(CleanupStage::outputs, "stderr writer did not finish");
    result_.cleanup.descendants_gone = descendants_gone_;

    const Millis end = exit_at_.count() != 0 ? exit_at_ : now;
    if (exec_at_.count() != 0)
        result_.timing.execution =
            std::chrono::duration_cast<std::chrono::microseconds>(end - exec_at_);
    result_.timing.total = std::chrono::duration_cast<std::chrono::microseconds>(now - created_at_);

    finished_ = true;
    phase_ = Phase::finished;
    return {Completed{}};
}

void ExecutionCore::add_cleanup_failure(CleanupStage stage, std::string detail) {
    result_.cleanup.failures.push_back(CleanupFailure{stage, std::move(detail)});
}

} // namespace sandbox
