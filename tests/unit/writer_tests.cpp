#include "sandbox/runtime/bounded_writer.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace sandbox;
using namespace std::chrono_literals;
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("writer assertion failed");
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::string bytes;
};
struct GatedSink : OutputSink {
    std::shared_ptr<Gate> gate;
    explicit GatedSink(std::shared_ptr<Gate> g) : gate(std::move(g)) {}
    std::size_t write(std::span<const char> bytes) override {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        if (!gate->cv.wait_for(lock, 3s, [&] { return gate->release; }))
            throw std::runtime_error("test gate timed out");
        // Deliberate short writes exercise byte accounting and stream ordering.
        auto count = std::min<std::size_t>(2, bytes.size());
        gate->bytes.append(bytes.data(), count);
        return count;
    }
};
struct BrokenSink : OutputSink {
    int calls = 0;
    std::size_t write(std::span<const char> bytes) override {
        if (calls++)
            throw std::runtime_error("injected disk failure");
        return std::min<std::size_t>(2, bytes.size());
    }
};
int main() {
    try {
        auto gate = std::make_shared<Gate>();
        std::atomic<unsigned> notified{0};
        BoundedWriter writer(std::make_unique<GatedSink>(gate), 8, [&] { ++notified; });
        require(writer.try_submit(std::vector<char>{'a', 'b', 'c', 'd'}));
        {
            std::unique_lock lock(gate->mutex);
            require(gate->cv.wait_for(lock, 2s, [&] { return gate->entered; }));
        }
        require(writer.try_submit(std::vector<char>{'e', 'f', 'g', 'h'}));
        std::vector<char> rejected{'x'};
        require(!writer.try_submit(std::move(rejected)) && rejected.size() == 1);
        require(writer.status().outstanding == 8 && writer.available() == 0);
        writer.close();
        writer.close();
        require(!writer.status().finished); // EOF/close is not write completion
        require(!writer.try_submit(std::move(rejected)));
        {
            std::lock_guard lock(gate->mutex);
            gate->release = true;
        }
        gate->cv.notify_all();
        writer.join();
        auto status = writer.status();
        require(status.finished && status.error.empty() && status.written == 8 && status.peak == 8);
        require(gate->bytes == "abcdefgh" && notified >= 3);
        BoundedWriter broken(std::make_unique<BrokenSink>(), 8, [] {});
        require(broken.try_submit(std::vector<char>{'1', '2', '3', '4'}));
        broken.close();
        broken.join();
        require(broken.status().finished && broken.status().written == 2 &&
                !broken.status().error.empty());
        require(broken.available() == 0 && !broken.try_submit(std::vector<char>{'x'}));
        // Shutdown of an empty queue must join without waiting for a data notification.
        {
            BoundedWriter empty(std::make_unique<BrokenSink>(), 8, [] {});
        }
        std::cout
            << "bounded writer ownership, backpressure, short write, failure and close passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
