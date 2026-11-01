#include "sandbox/core/prepare.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace sandbox {

const char *prepare_step_name(PrepareStep step) noexcept {
    switch (step) {
    case PrepareStep::state_root:
        return "state_root";
    case PrepareStep::control_socket:
        return "control_socket";
    case PrepareStep::cgroup:
        return "cgroup";
    case PrepareStep::output_capture:
        return "output_capture";
    case PrepareStep::input_snapshot:
        return "input_snapshot";
    case PrepareStep::launcher:
        return "launcher";
    case PrepareStep::namespaces:
        return "namespaces";
    case PrepareStep::id_map:
        return "id_map";
    case PrepareStep::cgroup_attach:
        return "cgroup_attach";
    case PrepareStep::rootfs:
        return "rootfs";
    case PrepareStep::mounts:
        return "mounts";
    case PrepareStep::security:
        return "security";
    case PrepareStep::ready:
        return "ready";
    }
    return "unknown";
}

CleanupStage prepare_step_stage(PrepareStep step) noexcept {
    switch (step) {
    case PrepareStep::state_root:
        return CleanupStage::workspace;
    case PrepareStep::control_socket:
        return CleanupStage::control_socket;
    case PrepareStep::cgroup:
        return CleanupStage::cgroup;
    case PrepareStep::output_capture:
        return CleanupStage::outputs;
    case PrepareStep::input_snapshot:
        return CleanupStage::workspace;
    case PrepareStep::launcher:
        return CleanupStage::launcher;
    case PrepareStep::namespaces:
    case PrepareStep::id_map:
    case PrepareStep::security:
    case PrepareStep::ready:
        return CleanupStage::init;
    case PrepareStep::cgroup_attach:
        return CleanupStage::cgroup;
    case PrepareStep::rootfs:
    case PrepareStep::mounts:
        return CleanupStage::mounts;
    }
    return CleanupStage::init;
}

PrepareSequence::PrepareSequence(std::vector<PrepareStepSpec> steps) : steps_(std::move(steps)) {
    if (steps_.empty())
        throw std::invalid_argument("prepare sequence must contain at least one step");
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        if (!steps_[i].apply)
            throw std::invalid_argument("prepare step has no apply action");
        if (i > 0 && static_cast<int>(steps_[i].step) <= static_cast<int>(steps_[i - 1].step))
            throw std::invalid_argument("prepare steps must be unique and in canonical order");
    }
}

PrepareOutcome PrepareSequence::run() {
    PrepareOutcome outcome;
    outcome.completed.reserve(steps_.size());
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        const auto &spec = steps_[i];
        try {
            spec.apply();
        } catch (const std::exception &failure) {
            outcome.failed_at = spec.step;
            outcome.reason = failure.what();
            break;
        } catch (...) {
            outcome.failed_at = spec.step;
            outcome.reason = "unknown preparation error";
            break;
        }
        outcome.completed.push_back(spec.step);
    }
    if (!outcome.failed_at) {
        outcome.ok = true;
        return outcome;
    }
    // Reverse rollback. Completion is always a prefix of the sequence, so the
    // undo set is simply steps_[0, completed). Each undo failure is recorded
    // against its own stage and does not abort the remaining undos.
    for (std::size_t i = outcome.completed.size(); i-- > 0;) {
        const auto &spec = steps_[i];
        if (!spec.undo)
            continue;
        try {
            spec.undo();
        } catch (const std::exception &failure) {
            outcome.rollback_failures.push_back({prepare_step_stage(spec.step), failure.what()});
        } catch (...) {
            outcome.rollback_failures.push_back(
                {prepare_step_stage(spec.step), "unknown rollback error"});
        }
    }
    return outcome;
}

std::string describe(const PrepareOutcome &outcome) {
    if (outcome.ok)
        return "preparation completed";
    std::string text = "preparation failed at ";
    text += outcome.failed_at ? prepare_step_name(*outcome.failed_at) : "unknown step";
    text += ": ";
    text += outcome.reason;
    if (!outcome.completed.empty()) {
        text += " (undone:";
        for (auto step : outcome.completed) {
            text += ' ';
            text += prepare_step_name(step);
        }
        text += ')';
    }
    if (!outcome.rollback_failures.empty())
        text += " (" + std::to_string(outcome.rollback_failures.size()) + " rollback failure(s))";
    return text;
}

} // namespace sandbox
