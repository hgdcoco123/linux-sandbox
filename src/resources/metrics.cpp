#include "sandbox/metrics.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace sandbox {
namespace {
std::string read_metric_file(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open metric: " + file.filename().string());
    std::string result;
    char data[4096];
    while (input.read(data, sizeof(data)) || input.gcount() > 0) {
        result.append(data, static_cast<std::size_t>(input.gcount()));
        if (result.size() > 1024 * 1024)
            throw std::runtime_error("metric exceeds 1MiB");
    }
    if (input.bad())
        throw std::runtime_error("cannot read metric: " + file.filename().string());
    return result;
}
class CounterFile final : public IMetricCollector {
  public:
    explicit CounterFile(std::string file) : file_(std::move(file)) {}
    std::string name() const override {
        return file_;
    }
    Json collect(const fs::path &root) const override {
        return parse_counter_file(read_metric_file(root / file_));
    }

  private:
    std::string file_;
};
class ScalarFile final : public IMetricCollector {
  public:
    ScalarFile(std::string file, std::string field)
        : file_(std::move(file)), field_(std::move(field)) {}
    std::string name() const override {
        return field_;
    }
    Json collect(const fs::path &root) const override {
        return parse_unsigned_counter(read_metric_file(root / file_));
    }

  private:
    std::string file_, field_;
};
class IoStat final : public IMetricCollector {
  public:
    std::string name() const override {
        return "io.stat";
    }
    Json collect(const fs::path &root) const override {
        return parse_io_stat(read_metric_file(root / "io.stat"));
    }
};
std::optional<std::uint64_t> counter(const Json &root, std::initializer_list<const char *> path) {
    const Json *value = &root;
    for (auto name : path) {
        if (!value->is_object() || !value->contains(name))
            return std::nullopt;
        value = &value->at(name);
    }
    if (value->is_number_unsigned())
        return value->get<std::uint64_t>();
    if (value->is_number_integer() && value->get<std::int64_t>() >= 0)
        return value->get<std::uint64_t>();
    return std::nullopt;
}
template <class T> Json optional_json(const std::optional<T> &v) {
    return v ? Json(*v) : Json(nullptr);
}
template <class T> void update_max(std::optional<T> &max, const std::optional<T> &value) {
    if (value && (!max || *value > *max))
        max = value;
}
} // namespace
std::uint64_t parse_unsigned_counter(const std::string &text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        throw std::runtime_error("empty counter");
    const auto end = text.find_last_not_of(" \t\r\n") + 1;
    std::uint64_t value = 0;
    const auto [last, error] = std::from_chars(text.data() + begin, text.data() + end, value);
    if (error != std::errc{} || last != text.data() + end)
        throw std::runtime_error("invalid unsigned counter");
    return value;
}
Json parse_counter_file(const std::string &text) {
    Json result = Json::object();
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key, value, extra;
        if (!(fields >> key))
            continue;
        if (!(fields >> value) || (fields >> extra) || result.contains(key))
            throw std::runtime_error("malformed/duplicate counter line");
        result[key] = parse_unsigned_counter(value);
    }
    if (result.empty())
        throw std::runtime_error("empty counter file");
    return result;
}
Json parse_io_stat(const std::string &text) {
    Json devices = Json::object();
    Json totals = {{"rbytes", std::uint64_t{0}},
                   {"wbytes", std::uint64_t{0}},
                   {"rios", std::uint64_t{0}},
                   {"wios", std::uint64_t{0}}};
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string device, field;
        if (!(fields >> device))
            continue;
        const auto colon = device.find(':');
        if (colon == std::string::npos || devices.contains(device))
            throw std::runtime_error("invalid/duplicate I/O device");
        parse_unsigned_counter(device.substr(0, colon));
        parse_unsigned_counter(device.substr(colon + 1));
        Json values = Json::object();
        while (fields >> field) {
            const auto split = field.find('=');
            if (split == std::string::npos || split == 0)
                throw std::runtime_error("invalid I/O field");
            const auto key = field.substr(0, split);
            if (values.contains(key))
                throw std::runtime_error("duplicate I/O field");
            values[key] = parse_unsigned_counter(field.substr(split + 1));
        }
        for (auto it = totals.begin(); it != totals.end(); ++it) {
            if (!values.contains(it.key()))
                throw std::runtime_error("missing I/O counter: " + it.key());
            const auto n = values.at(it.key()).get<std::uint64_t>();
            const auto old = it.value().get<std::uint64_t>();
            if (n > std::numeric_limits<std::uint64_t>::max() - old)
                throw std::runtime_error("I/O counter sum overflow");
            it.value() = old + n;
        }
        devices[device] = values;
    }
    return {{"devices", devices}, {"totals", totals}};
}
void MetricRegistry::add(std::unique_ptr<IMetricCollector> collector) {
    if (!collector)
        throw std::invalid_argument("null metric collector");
    if (collector->name().empty() || collector->name() == "scope" ||
        collector->name() == "unavailable" || collector->name() == "sampling")
        throw std::invalid_argument("reserved/empty metric name");
    for (const auto &existing : collectors_)
        if (existing->name() == collector->name())
            throw std::invalid_argument("duplicate metric name");
    collectors_.push_back(std::move(collector));
}
Json MetricRegistry::collect(const fs::path &root) const {
    Json result = {{"scope", "init_and_target_descendants"}};
    for (const auto &c : collectors_) {
        try {
            result[c->name()] = c->collect(root);
        } catch (const std::exception &e) {
            result[c->name()] = nullptr;
            result["unavailable"][c->name()] = e.what();
        }
    }
    return result;
}
MetricRegistry MetricRegistry::defaults() {
    MetricRegistry r;
    for (auto name : {"cpu.stat", "memory.events.local", "pids.events"})
        r.add(std::make_unique<CounterFile>(name));
    r.add(std::make_unique<ScalarFile>("memory.peak", "memory_peak_bytes"));
    r.add(std::make_unique<ScalarFile>("memory.current", "memory_current_bytes"));
    r.add(std::make_unique<ScalarFile>("pids.current", "pids_current"));
    r.add(std::make_unique<IoStat>());
    return r;
}
Json MetricSample::json() const {
    return {{"elapsed_ms", elapsed_ms},
            {"cpu_percent", optional_json(cpu_percent)},
            {"read_bytes_per_sec", optional_json(read_bytes_per_sec)},
            {"write_bytes_per_sec", optional_json(write_bytes_per_sec)},
            {"memory_current_bytes", optional_json(memory_current_bytes)},
            {"memory_peak_bytes", optional_json(memory_peak_bytes)},
            {"pids_current", optional_json(pids_current)},
            {"rate_notes", rate_notes}};
}
MetricsSampler::MetricsSampler(fs::path root, MetricRegistry registry, std::uint64_t interval,
                               std::size_t capacity)
    : cgroup_(std::move(root)), registry_(std::move(registry)), interval_ms_(interval),
      history_limit_(capacity) {
    if (interval == 0 || capacity == 0 || capacity > 1024)
        throw std::invalid_argument("invalid sampling interval/history capacity");
}
bool MetricsSampler::sample(std::uint64_t now, bool force) {
    if (previous_at_ && now < *previous_at_)
        throw std::invalid_argument("sampling clock moved backwards");
    if (!force && previous_at_ && now - *previous_at_ < interval_ms_)
        return false;
    auto raw = registry_.collect(cgroup_);
    if (!first_at_)
        first_at_ = now;
    MetricSample s;
    s.elapsed_ms = now - *first_at_;
    s.memory_current_bytes = counter(raw, {"memory_current_bytes"});
    s.memory_peak_bytes = counter(raw, {"memory_peak_bytes"});
    s.pids_current = counter(raw, {"pids_current"});
    auto rate = [&](std::initializer_list<const char *> path, const char *name,
                    double scale) -> std::optional<double> {
        auto current = counter(raw, path), old = counter(previous_raw_, path);
        if (!previous_at_ || now == *previous_at_ || !current || !old)
            return std::nullopt;
        if (*current < *old) {
            s.rate_notes.push_back(std::string(name) + ": counter_reset");
            return std::nullopt;
        }
        return static_cast<double>(*current - *old) * scale /
               static_cast<double>(now - *previous_at_);
    };
    s.cpu_percent = rate({"cpu.stat", "usage_usec"}, "cpu", 0.1);
    s.read_bytes_per_sec = rate({"io.stat", "totals", "rbytes"}, "read", 1000.0);
    s.write_bytes_per_sec = rate({"io.stat", "totals", "wbytes"}, "write", 1000.0);
    update_max(max_cpu_percent_, s.cpu_percent);
    update_max(max_sampled_memory_, s.memory_current_bytes);
    update_max(max_sampled_pids_, s.pids_current);
    history_.push_back(std::move(s));
    if (history_.size() > history_limit_)
        history_.pop_front();
    ++total_samples_;
    previous_at_ = now;
    previous_raw_ = raw;
    latest_raw_ = std::move(raw);
    return true;
}
Json MetricsSampler::live_snapshot() const {
    return {{"scope", "init_and_target_descendants"},
            {"samples_taken", total_samples_},
            {"latest", history_.empty() ? Json(nullptr) : history_.back().json()},
            {"unavailable", latest_raw_.value("unavailable", Json::object())}};
}
Json MetricsSampler::final_report() const {
    auto result = latest_raw_;
    Json history = Json::array();
    for (const auto &s : history_)
        history.push_back(s.json());
    result["sampling"] = {
        {"interval_ms", interval_ms_},
        {"samples_taken", total_samples_},
        {"history_limit", history_limit_},
        {"dropped_samples", total_samples_ - history_.size()},
        {"cpu_percent_convention", "one_logical_cpu_is_100_percent"},
        {"baseline", "first sample after init resource attachment, including environment setup"},
        {"max_cpu_percent", optional_json(max_cpu_percent_)},
        {"max_sampled_memory_bytes", optional_json(max_sampled_memory_)},
        {"max_sampled_pids", optional_json(max_sampled_pids_)},
        {"history", history}};
    return result;
}
} // namespace sandbox
