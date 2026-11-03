#pragma once
#include "sandbox/model/config.hpp"
#include <deque>
#include <memory>
#include <optional>
namespace sandbox {
// Trusted host-side extension. Collectors cannot control the target lifecycle.
class IMetricCollector {
  public:
    virtual ~IMetricCollector() = default;
    virtual std::string name() const = 0;
    virtual Json collect(const fs::path &cgroup) const = 0;
};
class MetricRegistry {
  public:
    void add(std::unique_ptr<IMetricCollector>);
    Json collect(const fs::path &) const;
    static MetricRegistry defaults();

  private:
    std::vector<std::unique_ptr<IMetricCollector>> collectors_;
};
Json parse_counter_file(const std::string &text);
Json parse_io_stat(const std::string &text);
std::uint64_t parse_unsigned_counter(const std::string &text);

struct MetricSample {
    std::uint64_t elapsed_ms = 0;
    std::optional<double> cpu_percent, read_bytes_per_sec, write_bytes_per_sec;
    std::optional<std::uint64_t> memory_current_bytes, memory_peak_bytes, pids_current;
    std::vector<std::string> rate_notes;
    Json json() const;
};

// One object per RunSession; neither collectors nor counter baselines are shared.
class MetricsSampler {
  public:
    MetricsSampler(fs::path cgroup, MetricRegistry registry, std::uint64_t interval_ms = 250,
                   std::size_t history_limit = 256);
    bool sample(std::uint64_t monotonic_ms, bool force = false);
    Json live_snapshot() const;
    Json final_report() const;

  private:
    fs::path cgroup_;
    MetricRegistry registry_;
    std::uint64_t interval_ms_, total_samples_ = 0;
    std::size_t history_limit_;
    std::optional<std::uint64_t> first_at_, previous_at_;
    Json previous_raw_ = Json::object(), latest_raw_ = Json::object();
    std::deque<MetricSample> history_;
    std::optional<double> max_cpu_percent_;
    std::optional<std::uint64_t> max_sampled_memory_, max_sampled_pids_;
};
} // namespace sandbox
