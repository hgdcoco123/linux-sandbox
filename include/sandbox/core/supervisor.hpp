#pragma once
// The execution core: a deterministic state machine for one target process.
//
// It owns no file descriptors and declares no syscalls. Callers feed it facts
// (environment ready, main exited, output finished, tick) and receive actions
// (signal the group, kill it, force the drain, finish). That keeps the whole
// single-run/session stop ladder testable without a kernel, and makes both
// controllers share exactly one implementation of it.
#include "sandbox/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sandbox {

using Millis = std::chrono::milliseconds;

enum class Phase { idle, starting, running, stopping, draining, finished, failed };
const char *phase_name(Phase) noexcept;

enum class StopIntent { cancel, timeout, session_closed };
const char *stop_intent_name(StopIntent) noexcept;

struct SupervisionLimits {
    Millis startup{5000};   // created -> environment ready
    Millis wall{30000};     // exec -> target must be gone
    Millis grace{500};      // SIGTERM -> SIGKILL
    Millis drain{5000};     // exit -> outputs flushed
    Millis settle{4000};    // exit -> descendants gone
};

struct OutputStats {
    std::uint64_t saved_bytes = 0;
    std::uint64_t discarded_bytes = 0;
    std::uint64_t failed_bytes = 0;
    std::uint64_t queue_peak_bytes = 0;
    std::uint64_t backpressure_events = 0;
    std::string write_error;
};

struct SignalTarget {
    int signal = 0;
};
struct KillTarget {};
struct ForceDrain {};
// Emitted exactly once; the caller stops feeding events afterwards.
struct Completed {};

using SupervisionAction = std::variant<SignalTarget, KillTarget, ForceDrain, Completed>;

class ExecutionCore {
  public:
    ExecutionCore(SupervisionLimits limits, Millis created_at);

    // Environment is prepared and the target may be started.
    std::vector<SupervisionAction> env_ready(Millis now);
    // The exec error pipe reported exec success.
    std::vector<SupervisionAction> exec_observed(Millis now);
    // The main target has been waited for. Only the first call before a stop
    // request can decide the outcome; later facts are diagnostics.
    std::vector<SupervisionAction> main_exited(std::optional<int> exit_code,
                                               std::optional<int> term_signal, Millis now);
    std::vector<SupervisionAction> stop_requested(StopIntent intent, Millis now);
    std::vector<SupervisionAction> oom_observed(Millis now);
    std::vector<SupervisionAction> descendants_empty(Millis now);
    std::vector<SupervisionAction> output_eof(bool is_stderr, Millis now);
    std::vector<SupervisionAction> output_finished(bool is_stderr, const OutputStats &, Millis now);
    std::vector<SupervisionAction> supervisor_failed(std::string reason, Millis now);
    std::vector<SupervisionAction> prepare_failed(std::string reason, Millis now);
    std::vector<SupervisionAction> tick(Millis now);

    // Cleanup facts that the runtime learns after the target result is sealed.
    void mark_launcher_reaped() noexcept { result_.cleanup.launcher_reaped = true; }
    void mark_cgroup_removed() noexcept { result_.cleanup.cgroup_removed = true; }
    void mark_workspace_removed() noexcept { result_.cleanup.workspace_removed = true; }
    void add_cleanup_failure(CleanupStage, std::string detail);

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const ExecutionResult &result() const noexcept { return result_; }

  private:
    [[nodiscard]] bool outcome_decided() const noexcept { return outcome_.has_value(); }
    [[nodiscard]] bool outputs_finished() const noexcept { return out_finished_ && err_finished_; }
    void decide_outcome();
    void begin_drain(Millis now);
    std::vector<SupervisionAction> maybe_finish(Millis now);

    SupervisionLimits limits_;
    Millis created_at_;
    Phase phase_ = Phase::idle;

    Millis exec_at_{0};
    Millis exit_at_{0};
    Millis stop_at_{0};
    Millis drain_at_{0};

    std::optional<StopIntent> intent_;
    std::optional<int> natural_exit_;
    std::optional<int> natural_signal_;
    std::optional<int> stopped_signal_;
    bool oom_ = false;
    bool descendants_gone_ = false;
    bool out_eof_ = false, err_eof_ = false;
    bool out_finished_ = false, err_finished_ = false;
    bool killed_ = false;
    bool drain_forced_ = false;

    std::optional<ExecutionOutcome> outcome_;
    ExecutionResult result_;
    bool finished_ = false;
};

} // namespace sandbox
