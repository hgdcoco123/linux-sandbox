#pragma once
#include "sandbox/cgroup.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/launcher.hpp"
#include "sandbox/metrics.hpp"
#include "sandbox/output.hpp"
#include "sandbox/store.hpp"
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
    void on_launcher(Message);
    void on_init(Message);
    void apply(const std::vector<SupervisionAction> &);
    void observe_streams(Millis now);
    void request_cancel();
    void mark_stopping();
    void transition(RunState);
    void finalize();
    void sample_metrics(bool force = false);
    Json live_metrics() const;
    RunConfig config_;
    fs::path state_root_, launcher_path_;
    StateMachine state_;
    RunReport report_;
    std::unique_ptr<ExecutionCore> core_;
    std::unique_ptr<RunStore> store_;
    std::unique_ptr<ControlServer> control_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<CgroupV2> cgroup_;
    std::unique_ptr<MetricsSampler> sampler_;
    std::unique_ptr<LauncherClient> launcher_;
    std::unique_ptr<OutputCapture> stdout_, stderr_;
    std::uint64_t started_ = 0;
    bool init_created_ = false, env_ready_ = false, init_done_ = false, init_reaped_ = false;
    bool init_abnormal_ = false;
    bool launcher_channel_open_ = true, init_channel_open_ = true;
    bool children_seen_ = false;
    bool out_eof_seen_ = false, out_done_seen_ = false, err_eof_seen_ = false, err_done_seen_ = false;
};
} // namespace sandbox
