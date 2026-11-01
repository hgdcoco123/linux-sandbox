#pragma once
// How a single target process execution ended.
//
// Modeled as a closed sum type rather than a struct of optional fields: every
// alternative carries exactly the facts that alternative can have, and no
// alternative can fabricate an exit code. "We do not know what happened" is a
// first-class value (Indeterminate) instead of exit_code = nullopt.
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace sandbox {

// 0 is a legitimate value everywhere, so no alternative uses it as a sentinel.
inline constexpr int no_exit_code = -1;
inline constexpr int no_signal = 0;

struct Exited {
    int code = no_exit_code;
    friend bool operator==(const Exited &, const Exited &) = default;
};
struct Signaled {
    int signal = no_signal;
    friend bool operator==(const Signaled &, const Signaled &) = default;
};
struct TimedOut {
    friend bool operator==(const TimedOut &, const TimedOut &) = default;
};
// Why the command was stopped by the sandbox rather than by itself. The
// distinction is kept because "the caller cancelled this command" and "the
// session holding it went away" are different facts: one is retryable by the
// same caller, the other is not.
enum class CancelCause {
    requested,
    session_closed,
};
const char *cancel_cause_name(CancelCause) noexcept;

struct Cancelled {
    CancelCause cause = CancelCause::requested;
    friend bool operator==(const Cancelled &, const Cancelled &) = default;
};
// The memory controller killed the group. Kept apart from Signaled because the
// kernel-level evidence (cgroup oom_kill) is stronger than a signal number and
// the reported status differs.
struct Oom {
    friend bool operator==(const Oom &, const Oom &) = default;
};
// The launcher/init could not build the environment or start the target, so the
// target never ran to a conclusion of its own.
struct StartupFailed {
    std::string reason;
    // The exec failure behind the reason, when the kernel reported one. 0 is a
    // sentinel for "no errno recorded"; a real exec never fails with 0.
    int exec_errno = 0;
    friend bool operator==(const StartupFailed &, const StartupFailed &) = default;
};
// The supervisor itself failed. Never reported as a target exit.
struct SupervisionFailed {
    std::string reason;
    friend bool operator==(const SupervisionFailed &, const SupervisionFailed &) = default;
};
// Facts conflict or are missing; we decline to invent a result.
struct Indeterminate {
    std::string reason;
    friend bool operator==(const Indeterminate &, const Indeterminate &) = default;
};

using OutcomeVariant = std::variant<Exited, Signaled, TimedOut, Cancelled, Oom, StartupFailed,
                                    SupervisionFailed, Indeterminate>;

class ExecutionOutcome {
  public:
    ExecutionOutcome() : value_(Indeterminate{"no result recorded"}) {}

    template <class T> explicit ExecutionOutcome(T value) : value_(std::move(value)) {}

    [[nodiscard]] const OutcomeVariant &variant() const noexcept { return value_; }
    [[nodiscard]] bool is_success() const noexcept;
    [[nodiscard]] bool is_terminal_without_result() const noexcept;
    [[nodiscard]] const char *name() const noexcept;
    // Nullopt for outcomes that must not claim an exit status.
    [[nodiscard]] std::optional<int> exit_code() const noexcept;
    [[nodiscard]] std::optional<int> term_signal() const noexcept;
    // Human-readable detail for reports; empty when the outcome has none.
    [[nodiscard]] const std::string *reason() const noexcept;

    friend bool operator==(const ExecutionOutcome &a, const ExecutionOutcome &b) {
        return a.value_ == b.value_;
    }

  private:
    OutcomeVariant value_;
};

} // namespace sandbox
