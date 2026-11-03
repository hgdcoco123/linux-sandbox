// Portable tests for the execution core: the single stop ladder shared by
// one-shot runs and sessions. No syscalls, no kernel, deterministic clock.
#include "sandbox/core/supervisor.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace sandbox;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool has_completed(const std::vector<SupervisionAction> &actions) {
    for (const auto &action : actions)
        if (std::holds_alternative<Completed>(action))
            return true;
    return false;
}

bool has_signal(const std::vector<SupervisionAction> &actions, int signal) {
    for (const auto &action : actions)
        if (auto *value = std::get_if<SignalTarget>(&action))
            if (value->signal == signal)
                return true;
    return false;
}

bool has_kill(const std::vector<SupervisionAction> &actions) {
    for (const auto &action : actions)
        if (std::holds_alternative<KillTarget>(action))
            return true;
    return false;
}

bool has_force_drain(const std::vector<SupervisionAction> &actions) {
    for (const auto &action : actions)
        if (std::holds_alternative<ForceDrain>(action))
            return true;
    return false;
}

SupervisionLimits fast_limits() {
    SupervisionLimits limits;
    limits.startup = Millis{1000};
    limits.wall = Millis{10000};
    limits.grace = Millis{100};
    limits.drain = Millis{200};
    limits.settle = Millis{100};
    return limits;
}

OutputStats stats(std::uint64_t saved) {
    OutputStats value;
    value.saved_bytes = saved;
    return value;
}

// Drives a core to the point where both outputs and descendants are settled.
void settle_children(ExecutionCore &core, Millis when) {
    core.descendants_empty(when);
    core.output_eof(false, when);
    core.output_eof(true, when);
    core.output_finished(false, stats(10), when);
    core.output_finished(true, stats(0), when);
}

void test_normal_exit() {
    ExecutionCore core{fast_limits(), Millis{0}};
    check(core.phase() == Phase::starting, "core starts in starting phase");
    core.env_ready(Millis{10});
    check(core.phase() == Phase::running, "env ready moves to running");

    auto on_exit = core.main_exited(0, std::nullopt, Millis{100});
    check(has_signal(on_exit, 15), "main exit sweeps descendants with SIGTERM");
    check(!core.finished(), "a dead main process is not a finished execution");

    settle_children(core, Millis{150});
    check(core.finished(), "execution finishes once outcome, descendants and outputs settle");

    const auto &result = core.result();
    check(result.outcome.exit_code() == 0, "exit code preserved");
    check(result.out.saved_bytes == 10, "stdout stats attached");
    check(result.cleanup.complete(), "clean cleanup");
    check(verdict_of(result) == Verdict::ok, "verdict ok");
    check(outcome_exit_code(result.outcome) == 0, "cli code 0");
    check(std::chrono::duration_cast<Millis>(result.timing.execution).count() == 90,
          "execution time spans exec to exit");
    check(std::chrono::duration_cast<Millis>(result.timing.total).count() == 150,
          "total time spans created to finish");
}

void test_finish_requires_all_outputs() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.main_exited(0, std::nullopt, Millis{100});
    core.descendants_empty(Millis{120});
    core.output_eof(false, Millis{120});
    core.output_finished(false, stats(4), Millis{130});
    check(!core.finished(), "stdout alone is not enough");
    core.output_eof(true, Millis{130});
    check(!core.finished(), "eof is not the same fact as a finished writer");
    core.output_finished(true, stats(0), Millis{140});
    check(core.finished(), "both writers finished");
}

void test_timeout_ladder() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});

    auto after_wall = core.tick(Millis{10011});
    check(has_signal(after_wall, 15), "wall timeout sends SIGTERM");
    check(core.phase() == Phase::stopping, "phase is stopping");

    auto too_early = core.tick(Millis{10050});
    check(!has_kill(too_early), "no SIGKILL inside the grace period");

    auto after_grace = core.tick(Millis{10112});
    check(has_kill(after_grace), "SIGKILL after the grace period");

    core.main_exited(std::nullopt, 9, Millis{10200});
    settle_children(core, Millis{10300});
    check(core.finished(), "timed-out execution finishes");
    check(std::holds_alternative<TimedOut>(core.result().outcome.variant()),
          "SIGKILL from our own ladder is reported as timeout, not as a signal");
    check(outcome_exit_code(core.result().outcome) == 124, "timeout cli code");
}

void test_cancel_does_not_overwrite_confirmed_exit() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.main_exited(0, std::nullopt, Millis{20});
    auto late_cancel = core.stop_requested(StopIntent::cancel, Millis{25});
    (void)late_cancel;
    settle_children(core, Millis{30});
    check(core.result().outcome.exit_code() == 0,
          "a late cancel keeps the confirmed exit code");
    check(verdict_of(core.result()) == Verdict::ok, "late cancel does not poison the verdict");
}

void test_cancel_before_exit() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    auto on_cancel = core.stop_requested(StopIntent::cancel, Millis{20});
    check(has_signal(on_cancel, 15), "cancel asks the group to stop");
    auto again = core.stop_requested(StopIntent::timeout, Millis{30});
    check(again.empty(), "the first stop reason wins");
    core.main_exited(std::nullopt, 15, Millis{40});
    settle_children(core, Millis{50});
    check(std::holds_alternative<Cancelled>(core.result().outcome.variant()),
          "cancelled execution reports cancellation");
    check(outcome_exit_code(core.result().outcome) == 130, "cancel cli code");
}

void test_stuck_descendants() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.main_exited(0, std::nullopt, Millis{100});
    core.output_eof(false, Millis{110});
    core.output_eof(true, Millis{110});
    core.output_finished(false, stats(1), Millis{120});
    core.output_finished(true, stats(0), Millis{120});
    check(!core.finished(), "descendants are still alive");

    auto late = core.tick(Millis{500}); // past drain + settle
    check(has_completed(late), "deadline forces completion");
    const auto &result = core.result();
    check(result.target_succeeded(), "target itself still exited 0");
    check(!result.cleanup.complete(), "cleanup is reported incomplete");
    check(verdict_of(result) == Verdict::cleanup_incomplete, "verdict is cleanup_incomplete");
}

void test_output_never_finishes() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.main_exited(0, std::nullopt, Millis{100});
    core.descendants_empty(Millis{110});
    core.output_eof(false, Millis{110});
    core.output_eof(true, Millis{110});

    auto drain = core.tick(Millis{350}); // past drain deadline
    check(has_force_drain(drain), "drain deadline forces the writer to stop accepting");
    auto done = core.tick(Millis{500});
    check(has_completed(done), "execution still completes");
    check(!core.result().out.complete(), "stdout is reported incomplete");
    check(verdict_of(core.result()) == Verdict::cleanup_incomplete, "outputs failure is a cleanup verdict");
}

void test_supervision_failure() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.supervisor_failed("init died", Millis{20});
    check(core.phase() == Phase::failed, "supervisor failure marks the phase");
    settle_children(core, Millis{30});
    const auto &result = core.result();
    check(std::holds_alternative<SupervisionFailed>(result.outcome.variant()),
          "supervision failure is not reported as a target result");
    check(verdict_of(result) == Verdict::harness_error, "verdict is harness_error");
    check(outcome_exit_code(result.outcome) == 125, "harness cli code");
}

void test_prepare_failure() {
    ExecutionCore core{fast_limits(), Millis{0}};
    auto actions = core.prepare_failed("mount failed", Millis{5});
    check(has_completed(actions), "prepare failure completes immediately");
    const auto &result = core.result();
    check(std::holds_alternative<StartupFailed>(result.outcome.variant()), "startup failure outcome");
    check(outcome_exit_code(result.outcome) == 12, "startup cli code");
    check(result.cleanup.descendants_gone, "nothing was started, so nothing is left to reap");
}

void test_oom() {
    ExecutionCore core{fast_limits(), Millis{0}};
    core.env_ready(Millis{10});
    core.oom_observed(Millis{40});
    core.main_exited(std::nullopt, 9, Millis{50});
    settle_children(core, Millis{60});
    check(std::holds_alternative<Oom>(core.result().outcome.variant()), "oom outcome");
    check(outcome_exit_code(core.result().outcome) == 13, "oom cli code");
}

void test_startup_timeout() {
    ExecutionCore core{fast_limits(), Millis{0}};
    auto early = core.tick(Millis{500});
    check(early.empty(), "no startup timeout before the deadline");
    auto late = core.tick(Millis{1001});
    check(has_signal(late, 15), "startup timeout stops the group");
    core.tick(Millis{1110});
    core.descendants_empty(Millis{1120});
    core.output_eof(false, Millis{1120});
    core.output_eof(true, Millis{1120});
    core.output_finished(false, stats(0), Millis{1130});
    core.output_finished(true, stats(0), Millis{1130});
    check(core.finished(), "startup timeout eventually finishes");
    check(core.result().outcome.exit_code().has_value() == false, "startup failure has no exit code");
    check(outcome_exit_code(core.result().outcome) == 12, "startup timeout cli code");
}
} // namespace

int main() {
    test_normal_exit();
    test_finish_requires_all_outputs();
    test_timeout_ladder();
    test_cancel_does_not_overwrite_confirmed_exit();
    test_cancel_before_exit();
    test_stuck_descendants();
    test_output_never_finishes();
    test_supervision_failure();
    test_prepare_failure();
    test_oom();
    test_startup_timeout();
    if (failures != 0) {
        std::printf("%d supervisor check(s) failed\n", failures);
        return 1;
    }
    std::printf("execution core stop ladder and result assembly passed\n");
    return 0;
}
