#pragma once
#include "sandbox/cgroup.hpp"
#include "sandbox/launcher.hpp"
#include "sandbox/metrics.hpp"
#include "sandbox/output.hpp"
#include "sandbox/store.hpp"
#include <memory>
#include <spdlog/logger.h>
namespace sandbox {
class RunSession {
  public:
    RunSession(RunConfig config, fs::path state_root, fs::path launcher);
    RunReport run();

  private:
    void prepare();
    void supervise(SignalSource &);
    void on_launcher(Message);
    void on_init(Message);
    void request_stop(const std::string &);
    void transition(RunState);
    void finalize();
    void sample_metrics(bool force = false);
    Json live_metrics() const;
    RunConfig config_;
    fs::path state_root_, launcher_path_;
    StateMachine state_;
    RunReport report_;
    std::unique_ptr<RunStore> store_;
    std::unique_ptr<ControlServer> control_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<CgroupV2> cgroup_;
    std::unique_ptr<MetricsSampler> sampler_;
    std::unique_ptr<LauncherClient> launcher_;
    std::unique_ptr<OutputCapture> stdout_, stderr_;
    std::uint64_t started_ = 0, exec_at_ = 0, stop_at_ = 0;
    bool init_created_ = false, env_ready_ = false, init_done_ = false, init_reaped_ = false;
    bool init_abnormal_ = false;
    bool stopping_ = false, forced_ = false, launcher_channel_open_ = true,
         init_channel_open_ = true;
};
} // namespace sandbox
