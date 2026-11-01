#include "sandbox/core/outcome.hpp"

namespace sandbox {

const char *ExecutionOutcome::name() const noexcept {
    switch (value_.index()) {
    case 0:
        return "exited";
    case 1:
        return "signaled";
    case 2:
        return "timeout";
    case 3:
        return "cancelled";
    case 4:
        return "oom";
    case 5:
        return "startup_failure";
    case 6:
        return "supervision_failure";
    case 7:
        return "indeterminate";
    default:
        return "unknown";
    }
}

const char *cancel_cause_name(CancelCause cause) noexcept {
    switch (cause) {
    case CancelCause::requested:
        return "requested";
    case CancelCause::session_closed:
        return "session_closed";
    }
    return "requested";
}

bool ExecutionOutcome::is_success() const noexcept {
    auto *exited = std::get_if<Exited>(&value_);
    return exited != nullptr && exited->code == 0;
}

bool ExecutionOutcome::is_terminal_without_result() const noexcept {
    return std::holds_alternative<StartupFailed>(value_) ||
           std::holds_alternative<SupervisionFailed>(value_) ||
           std::holds_alternative<Indeterminate>(value_);
}

std::optional<int> ExecutionOutcome::exit_code() const noexcept {
    if (auto *exited = std::get_if<Exited>(&value_))
        return exited->code;
    return std::nullopt;
}

std::optional<int> ExecutionOutcome::term_signal() const noexcept {
    if (auto *signaled = std::get_if<Signaled>(&value_))
        return signaled->signal;
    return std::nullopt;
}

const std::string *ExecutionOutcome::reason() const noexcept {
    if (auto *failed = std::get_if<StartupFailed>(&value_))
        return &failed->reason;
    if (auto *failed = std::get_if<SupervisionFailed>(&value_))
        return &failed->reason;
    if (auto *unknown = std::get_if<Indeterminate>(&value_))
        return &unknown->reason;
    return nullptr;
}

} // namespace sandbox
