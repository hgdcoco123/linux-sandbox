#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
namespace sandbox {
class OutputSink {
  public:
    virtual ~OutputSink() = default;
    virtual std::size_t write(std::span<const char>) = 0;
};
struct WriterStatus {
    std::size_t outstanding = 0, peak = 0;
    std::uint64_t written = 0;
    bool finished = false;
    std::string error;
};
// One producer, one consumer. Capacity includes the in-flight block. No callbacks
// under the mutex; only the worker touches the sink. A rejected block is not moved.
class BoundedWriter {
  public:
    BoundedWriter(std::unique_ptr<OutputSink>, std::size_t capacity, std::function<void()> notify);
    ~BoundedWriter();
    BoundedWriter(const BoundedWriter &) = delete;
    BoundedWriter &operator=(const BoundedWriter &) = delete;
    bool try_submit(std::vector<char> &&);
    std::size_t available() const;
    WriterStatus status() const;
    void close();
    void join();

  private:
    void work() noexcept;
    void notify() noexcept;
    std::unique_ptr<OutputSink> sink_;
    const std::size_t capacity_;
    std::function<void()> notify_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<char>> queue_;
    WriterStatus status_;
    bool closing_ = false;
    std::thread worker_; // destroyed/joined before sink, notifier and synchronization
};
} // namespace sandbox
