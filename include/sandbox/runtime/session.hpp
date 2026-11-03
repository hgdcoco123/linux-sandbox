#pragma once
#include "sandbox/resources/cgroup.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/runtime/launcher.hpp"
#include "sandbox/resources/metrics.hpp"
#include "sandbox/runtime/output.hpp"
#include "sandbox/storage/store.hpp"
#include <memory>
#include <spdlog/logger.h>
namespace sandbox {
// Single-run controller: owns the environment, but every lifecycle decision is
// delegated to ExecutionCore. There is no timeout, grace or completion rule in
// this class; it only feeds facts and applies the returned actions.
class RunSession {
  public:
    RunSession(RunConfig config, fs::path state_root, fs::path launcher);
    RunReport run();

  private:
    void prepare();
    void supervise(SignalSource &);
    void on_launcher(ControlEnvelope);
    void on_init(ControlEnvelope);
    void apply(const std::vector<SupervisionAction> &);
    void observe_streams(Millis now);
    void request_cancel();
    void mark_stopping();
    void transition(RunState);
    void finalize();
    void sample_metrics(bool force = false);
    Json live_metrics() const;
    void record_workspace_export(bool attempted, bool ok, std::uint64_t bytes,
                                 std::uint32_t entries, const std::string &error);
    void publish_workspace(std::uint32_t entries, std::uint64_t bytes);
    // Adopt an export that landed on disk but whose answer never arrived.
    bool adopt_landed_export();
    // True while the run still owes the caller an answer about its artifacts.
    bool workspace_pending() const {
        return quota_workspace_ && !workspace_answered_ && !init_done_;
    }
    RunConfig config_;
    fs::path state_root_, launcher_path_;
    StateMachine state_;
    RunReport report_;
    std::unique_ptr<ExecutionCore> core_;
    // Budget for an unconfirmed stop; the loop reports instead of spinning.
    Millis stop_started_{0};
    std::int64_t stop_budget_ms_ = 2000;
    Millis trace_at_{0};
    std::unique_ptr<RunStore> store_;
    std::unique_ptr<ControlServer> control_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<CgroupV2> cgroup_;
    std::unique_ptr<MetricsSampler> sampler_;
    std::unique_ptr<LauncherClient> launcher_;
    std::unique_ptr<OutputCapture> stdout_, stderr_;
    // Snapshot copies of /input made by the input_snapshot step, so a failed
    // preparation can take them back.
    std::vector<fs::path> input_snapshots_;
    // Target-side write ends, held only between pipe creation and launcher spawn.
    UniqueFd pipe_out_, pipe_err_;
    // Decision A: in isolated mode /output is a quota tmpfs owned by the sandbox.
    // `transfer_` is the descriptor directory init exports into, and
    // `output_destination_` is the caller's directory the export is published to.
    fs::path transfer_, output_destination_;
    bool quota_workspace_ = false, workspace_answered_ = false;
    bool workspace_wait_seen_ = false;
    Millis workspace_wait_started_{0};
    std::uint64_t workspace_budget_ms_ = 30000;
    std::uint64_t started_ = 0;
    bool init_created_ = false, env_ready_ = false, init_done_ = false, init_reaped_ = false;
    bool init_abnormal_ = false;
    bool launcher_channel_open_ = true, init_channel_open_ = true;
    bool children_seen_ = false;
    bool out_eof_seen_ = false, out_done_seen_ = false, err_eof_seen_ = false, err_done_seen_ = false;
};
} // namespace sandbox
