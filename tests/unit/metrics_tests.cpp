#include "sandbox/resources/metrics.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace sandbox;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception &) {
        threw = true;
    }
    require(threw, "expected rejection");
}
void near(const Json &value, double expected, const char *message) {
    require(value.is_number() && std::abs(value.get<double>() - expected) < 0.000001, message);
}
struct FakeCgroup {
    fs::path root;
    FakeCgroup() {
        root =
            fs::temp_directory_path() / ("sbx-metrics-" + std::to_string(std::random_device{}()));
        if (!fs::create_directory(root))
            throw std::runtime_error("fixture collision");
    }
    ~FakeCgroup() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void write(const char *file, const std::string &value) {
        std::ofstream out(root / file, std::ios::binary | std::ios::trunc);
        out << value;
        if (!out)
            throw std::runtime_error("fixture write");
    }
    void values(std::uint64_t cpu, std::uint64_t read, std::uint64_t written,
                std::uint64_t memory = 4096, std::uint64_t pids = 2) {
        write("cpu.stat", "usage_usec " + std::to_string(cpu));
        write("memory.current", std::to_string(memory));
        write("memory.peak", "99999");
        write("pids.current", std::to_string(pids));
        write("memory.events.local", "oom 0\noom_kill 0\n");
        write("pids.events", "max 0\n");
        write("io.stat", "8:0 rbytes=" + std::to_string(read) +
                             " wbytes=" + std::to_string(written) + " rios=1 wios=2");
    }
};
class Throws final : public IMetricCollector {
  public:
    std::string name() const override {
        return "broken";
    }
    Json collect(const fs::path &) const override {
        throw std::runtime_error("injected");
    }
};
} // namespace
int main() {
    try {
        require(parse_counter_file("usage_usec 123")["usage_usec"] == 123,
                "last line without newline");
        require(parse_unsigned_counter(" 18446744073709551615\n") ==
                    std::numeric_limits<std::uint64_t>::max(),
                "uint64 max");
        for (auto text : {"", "-1", "1junk", "18446744073709551616"})
            rejects([&] { parse_unsigned_counter(text); });
        for (auto text : {"a 1 extra", "a 1\na 2", "a", "a -1"})
            rejects([&] { parse_counter_file(text); });
        auto io = parse_io_stat(
            "8:0 rbytes=10 wbytes=20 rios=1 wios=2\n8:16 wios=3 rios=4 wbytes=7 rbytes=5 dbytes=0");
        require(io["totals"]["rbytes"] == 15 && io["totals"]["wbytes"] == 27,
                "aggregate devices/field order");
        require(parse_io_stat("")["totals"]["rbytes"] == 0, "empty io.stat is zero");
        rejects([] { parse_io_stat("8:0 rbytes=2 wbytes=4"); });
        rejects([] {
            parse_io_stat("8:0 rbytes=18446744073709551615 wbytes=0 rios=0 wios=0\n8:1 rbytes=1 "
                          "wbytes=0 rios=0 wios=0");
        });
        FakeCgroup fake;
        const std::uint64_t big = (1ULL << 60);
        fake.values(big, 100, 200);
        auto registry = MetricRegistry::defaults();
        registry.add(std::make_unique<Throws>());
        MetricsSampler sampler(fake.root, std::move(registry), 250, 2);
        require(sampler.sample(1000), "first sample");
        auto live = sampler.live_snapshot();
        require(live["latest"]["cpu_percent"].is_null(), "first rate unavailable, not zero");
        require(live["unavailable"].contains("broken"), "partial collector failure retained");
        require(!sampler.sample(1100), "sampling interval independent of event loop");
        fake.values(big + 500000, 1124, 2248, 8192, 3);
        sampler.sample(1500);
        live = sampler.live_snapshot();
        near(live["latest"]["cpu_percent"], 100, "integer delta preserves large CPU counters");
        near(live["latest"]["read_bytes_per_sec"], 2048, "read bytes/s");
        near(live["latest"]["write_bytes_per_sec"], 4096, "write bytes/s");
        require(live["latest"]["memory_current_bytes"] == 8192, "memory current");
        MetricsSampler other(fake.root, MetricRegistry::defaults(), 250, 2);
        other.sample(1500);
        require(other.live_snapshot()["latest"]["cpu_percent"].is_null(),
                "independent run baseline");
        fake.values(1, 0, 0, 1024, 1);
        sampler.sample(1750);
        live = sampler.live_snapshot();
        require(live["latest"]["cpu_percent"].is_null() && !live["latest"]["rate_notes"].empty(),
                "counter reset");
        auto report = sampler.final_report();
        require(report["sampling"]["history"].size() == 2 &&
                    report["sampling"]["dropped_samples"] == 1,
                "bounded history");
        require(report["sampling"]["max_sampled_memory_bytes"] == 8192 &&
                    report["memory_peak_bytes"] == 99999,
                "sample peak vs kernel peak");
        rejects([&] { sampler.sample(1700, true); });
        sampler.sample(1750, true);
        require(sampler.live_snapshot()["latest"]["cpu_percent"].is_null(),
                "zero duration is unavailable");
        fs::remove(fake.root / "cpu.stat");
        fake.write("memory.current", "invalid");
        sampler.sample(2000);
        live = sampler.live_snapshot();
        require(live["latest"]["cpu_percent"].is_null() &&
                    live["latest"]["memory_current_bytes"].is_null(),
                "missing/invalid metric is not zero");
        require(live["unavailable"].contains("cpu.stat") &&
                    live["unavailable"].contains("memory_current_bytes"),
                "field-level errors");
        fake.values(1000, 10, 20);
        sampler.sample(2250);
        require(sampler.live_snapshot()["latest"]["cpu_percent"].is_null(),
                "reappearance establishes new baseline");
        fake.values(501000, 20, 40);
        sampler.sample(2500);
        near(sampler.live_snapshot()["latest"]["cpu_percent"], 200,
             "multi-core can exceed 100 percent");
        auto serialized = sampler.final_report().dump();
        require(Json::parse(serialized).is_object(), "finite JSON report");
        auto defaults = MetricRegistry::defaults();
        rejects([&] { defaults.add(nullptr); });
        rejects([&] { MetricsSampler bad(fake.root, MetricRegistry::defaults(), 0); });
        std::cout << "metrics parser/sampling tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
