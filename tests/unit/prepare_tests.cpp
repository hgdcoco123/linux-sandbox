// Portable tests for ordered preparation and reverse rollback.
#include "sandbox/core/prepare.hpp"

#include <cstdio>
#include <stdexcept>
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

template <class F> void check_invalid(F &&body, const char *what) {
    try {
        body();
        std::printf("FAIL: %s (accepted)\n", what);
        ++failures;
    } catch (const std::invalid_argument &) {
    } catch (const std::exception &error) {
        std::printf("FAIL: %s (wrong exception: %s)\n", what, error.what());
        ++failures;
    }
}

PrepareStepSpec spec(PrepareStep step, std::vector<std::string> &trace) {
    auto name = std::string(prepare_step_name(step));
    return PrepareStepSpec{step, [&trace, name] { trace.push_back("do:" + name); },
                           [&trace, name] { trace.push_back("undo:" + name); }};
}

void test_all_steps_succeed() {
    std::vector<std::string> trace;
    PrepareSequence sequence({spec(PrepareStep::state_root, trace), spec(PrepareStep::cgroup, trace),
                              spec(PrepareStep::launcher, trace), spec(PrepareStep::ready, trace)});
    auto outcome = sequence.run();
    check(outcome.ok, "successful preparation");
    check(!outcome.failed_at, "no failing step");
    check(outcome.completed.size() == 4, "all steps completed");
    check(trace == std::vector<std::string>({"do:state_root", "do:cgroup", "do:launcher", "do:ready"}),
          "steps ran in order and nothing was undone");
    check(describe(outcome) == "preparation completed", "describe for success");
}

void test_failure_rolls_back_in_reverse() {
    std::vector<std::string> trace;
    PrepareSequence sequence({
        spec(PrepareStep::state_root, trace),
        spec(PrepareStep::cgroup, trace),
        PrepareStepSpec{PrepareStep::launcher,
                        [&trace] {
                            trace.push_back("do:launcher");
                            throw std::runtime_error("clone3 denied");
                        },
                        [&trace] { trace.push_back("undo:launcher"); }},
        spec(PrepareStep::ready, trace),
    });
    auto outcome = sequence.run();
    check(!outcome.ok, "failed preparation");
    check(outcome.failed_at == PrepareStep::launcher, "failed at the launcher step");
    check(outcome.reason == "clone3 denied", "reason preserved");
    check(outcome.completed.size() == 2, "only the completed prefix is undone");
    // The failing step's own undo must not run: it never completed.
    check(trace == std::vector<std::string>({"do:state_root", "do:cgroup", "do:launcher",
                                             "undo:cgroup", "undo:state_root"}),
          "rollback runs strictly in reverse and skips the failed step");
    check(outcome.rollback_failures.empty(), "no rollback failures");
    check(describe(outcome).find("preparation failed at launcher") != std::string::npos,
          "describe names the failing step");
}

void test_rollback_continues_after_undo_failure() {
    std::vector<std::string> trace;
    PrepareSequence sequence({
        PrepareStepSpec{PrepareStep::state_root, [&trace] { trace.push_back("do:state_root"); },
                        [&trace] {
                            trace.push_back("undo:state_root");
                            throw std::runtime_error("directory busy");
                        }},
        PrepareStepSpec{PrepareStep::cgroup, [&trace] { trace.push_back("do:cgroup"); },
                        [&trace] {
                            trace.push_back("undo:cgroup");
                            throw std::runtime_error("cgroup busy");
                        }},
        PrepareStepSpec{PrepareStep::mounts,
                        [] { throw std::runtime_error("mount failed"); }, {}},
    });
    auto outcome = sequence.run();
    check(!outcome.ok, "preparation failed");
    check(outcome.completed.size() == 2, "two steps completed");
    check(trace == std::vector<std::string>({"do:state_root", "do:cgroup", "undo:cgroup",
                                             "undo:state_root"}),
          "undo continues in reverse even when an undo throws");
    check(outcome.rollback_failures.size() == 2, "both undo failures recorded");
    if (outcome.rollback_failures.size() == 2) {
        check(outcome.rollback_failures[0].stage == CleanupStage::cgroup, "cgroup undo failure stage");
        check(outcome.rollback_failures[0].detail == "cgroup busy", "undo failure detail");
        check(outcome.rollback_failures[1].stage == CleanupStage::workspace,
              "state root undo maps to workspace stage");
    }
}

void test_first_step_failure_has_nothing_to_undo() {
    std::vector<std::string> trace;
    PrepareSequence sequence({PrepareStepSpec{PrepareStep::state_root,
                                              [] { throw std::runtime_error("mkdir denied"); },
                                              [&trace] { trace.push_back("undo"); }},
                              spec(PrepareStep::cgroup, trace)});
    auto outcome = sequence.run();
    check(!outcome.ok, "first step failed");
    check(outcome.completed.empty(), "nothing completed");
    check(trace.empty(), "nothing undone and nothing after the failure ran");
}

void test_sparse_sequence() {
    // The process backend has no cgroup or id map: sparse sequences must work.
    std::vector<std::string> trace;
    PrepareSequence sequence({spec(PrepareStep::state_root, trace), spec(PrepareStep::output_capture, trace),
                              spec(PrepareStep::launcher, trace), spec(PrepareStep::ready, trace)});
    auto outcome = sequence.run();
    check(outcome.ok, "sparse sequence succeeds");
    check(outcome.completed.size() == 4, "sparse sequence ran all steps");
}

void test_rejected_sequences() {
    check_invalid([] { PrepareSequence sequence({}); }, "empty sequence");
    check_invalid(
        [] {
            PrepareSequence sequence({
                PrepareStepSpec{PrepareStep::cgroup, [] {}, [] {}},
                PrepareStepSpec{PrepareStep::state_root, [] {}, [] {}},
            });
        },
        "out-of-order sequence");
    check_invalid(
        [] {
            PrepareSequence sequence({
                PrepareStepSpec{PrepareStep::cgroup, [] {}, [] {}},
                PrepareStepSpec{PrepareStep::cgroup, [] {}, [] {}},
            });
        },
        "duplicate step");
    check_invalid(
        [] { PrepareSequence sequence({PrepareStepSpec{PrepareStep::cgroup, {}, [] {}}}); },
        "missing apply action");
}

void test_step_stage_mapping() {
    check(prepare_step_stage(PrepareStep::cgroup) == CleanupStage::cgroup, "cgroup stage");
    check(prepare_step_stage(PrepareStep::mounts) == CleanupStage::mounts, "mounts stage");
    check(prepare_step_stage(PrepareStep::output_capture) == CleanupStage::outputs, "output stage");
    check(prepare_step_stage(PrepareStep::control_socket) == CleanupStage::control_socket,
          "socket stage");
    check(prepare_step_stage(PrepareStep::launcher) == CleanupStage::launcher, "launcher stage");
}
} // namespace

int main() {
    test_all_steps_succeed();
    test_failure_rolls_back_in_reverse();
    test_rollback_continues_after_undo_failure();
    test_first_step_failure_has_nothing_to_undo();
    test_sparse_sequence();
    test_rejected_sequences();
    test_step_stage_mapping();
    if (failures != 0) {
        std::printf("%d prepare check(s) failed\n", failures);
        return 1;
    }
    std::printf("ordered preparation and reverse rollback passed\n");
    return 0;
}
