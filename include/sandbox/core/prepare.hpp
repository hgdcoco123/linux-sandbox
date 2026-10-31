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
