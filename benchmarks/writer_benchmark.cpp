#include "sandbox/runtime/bounded_writer.hpp"
#include "sandbox/model/config.hpp"
#include <chrono>
#include <iostream>
using namespace sandbox;
using namespace std::chrono;
struct SlowSink : OutputSink {
    std::size_t write(std::span<const char> bytes) override {
        std::this_thread::sleep_for(milliseconds(1));
        return bytes.size();
    }
};
int main() {
    constexpr int blocks = 64, block_size = 4096;
    std::vector<char> block(block_size, 'x');
    SlowSink sync;
    auto start = steady_clock::now();
    std::int64_t sync_max = 0;
    for (int i = 0; i < blocks; ++i) {
        auto before = steady_clock::now();
        sync.write(block);
        sync_max = std::max<std::int64_t>(
            sync_max, static_cast<std::int64_t>(
                          duration_cast<nanoseconds>(steady_clock::now() - before).count()));
    }
    auto sync_total = duration_cast<microseconds>(steady_clock::now() - start).count();
    BoundedWriter writer(std::make_unique<SlowSink>(), 65536, [] {});
    start = steady_clock::now();
    std::int64_t submit_max = 0;
    unsigned full = 0;
    for (int i = 0; i < blocks;) {
        auto copy = block;
        auto before = steady_clock::now();
        bool accepted = writer.try_submit(std::move(copy));
        submit_max = std::max<std::int64_t>(
            submit_max, static_cast<std::int64_t>(
                            duration_cast<nanoseconds>(steady_clock::now() - before).count()));
        if (accepted)
            ++i;
        else {
            ++full;
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
    writer.close();
    writer.join();
    std::cout << Json{{"workload", "64 x 4096 bytes; simulated 1ms sink per write"},
                      {"sync_total_us", sync_total},
                      {"async_total_us",
                       duration_cast<microseconds>(steady_clock::now() - start).count()},
                      {"sync_max_call_ns", sync_max},
                      {"async_max_submit_ns", submit_max},
                      {"queue_peak_bytes", writer.status().peak},
                      {"backpressure_retries", full}}
                     .dump(2)
              << '\n';
}
