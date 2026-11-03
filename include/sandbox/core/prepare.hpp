#pragma once
// Ordered preparation of an isolated execution, with reverse rollback.
//
// The sequence is fixed: a step may only be prepared after the steps it depends
// on. Registration enforces that order once, so the rollback path never has to
// reconstruct it from a pile of if-statements, and the report can say exactly
// how far preparation got before it failed.
#include "sandbox/core/result.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sandbox {

// Canonical order. Do not reorder: the numeric order is the dependency order.
enum class PrepareStep {
    state_root,
    control_socket,
    cgroup,
    output_capture,
    // Before the launcher, because the launcher is handed the mount sources: a
    // snapshot has to be on disk by then, and a bind would let the host keep
    // writing into a running sandbox.
    input_snapshot,
    launcher,
    namespaces,
    id_map,
    cgroup_attach,
    rootfs,
    mounts,
    security,
    ready,
};

const char *prepare_step_name(PrepareStep) noexcept;
// Which cleanup stage a failure of this step belongs to.
CleanupStage prepare_step_stage(PrepareStep) noexcept;

struct PrepareStepSpec {
    PrepareStep step;
    std::function<void()> apply;
    // Only called when `apply` completed. A step that fails halfway is
    // responsible for its own partial state; the journal does not guess.
    std::function<void()> undo;
};

// Contract for a step, in full. It is written down because the journal deliberately
// does not guess, and a step that quietly violates it leaks something nobody undoes.
//
//  - `apply` must be exception-safe. Only the *completed* prefix is undone, so a step
//    that throws owns whatever it already created. For the common "create a few
//    directories" shape use `create_directories_or_remove()`, which removes exactly
//    what that call created (reverse order, rmdir only) and keeps the original error.
//  - `undo` may assume `apply` returned, and should tolerate being run once. It must
//    also make an attempt at *every* resource it owns: stopping at the first failed
//    removal is how the rest of a rollback gets silently skipped. Use
//    `remove_all_collect()` (or the same "collect and report at the end" shape) so one
//    failure does not hide the others.
//  - A failed step may leave *diagnostics inside the run directory*: stdout.bin,
//    stderr.bin, internal.log, state.json, config.json, control.sock, and a partially
//    filled transfer/ or input/ snapshot. Those are kept on purpose so a failed run can
//    be diagnosed; a file that remains there is not by itself a leak.
//  - What a step must never leave behind is something that outlives the run: a cgroup,
//    a mount, a process, a socket outside the run directory, or a directory outside it.
//    If a step can create one of those, its `apply` has to clean up after itself or be
//    split so the resource-creating part is a step whose `undo` handles it.

struct PrepareOutcome {
    bool ok = false;
    std::optional<PrepareStep> failed_at;
    std::string reason;
    // Steps that completed, in execution order.
    std::vector<PrepareStep> completed;
    // Undo failures collected during the rollback, in rollback order.
    std::vector<CleanupFailure> rollback_failures;
};

class PrepareSequence {
  public:
    // Throws std::invalid_argument for duplicate, out-of-order or empty
    // sequences: a wrong preparation order is a programming error, not a
    // runtime condition.
    explicit PrepareSequence(std::vector<PrepareStepSpec> steps);

    // Runs the steps until one throws. On failure every completed step is
    // undone in reverse order; a failing undo is recorded and the rollback
    // continues, because leaving later resources behind is worse.
    [[nodiscard]] PrepareOutcome run();

    [[nodiscard]] const std::vector<PrepareStepSpec> &steps() const noexcept { return steps_; }

  private:
    std::vector<PrepareStepSpec> steps_;
};

// Convenience: the outcome as a single line for logs and reports.
[[nodiscard]] std::string describe(const PrepareOutcome &);

} // namespace sandbox
