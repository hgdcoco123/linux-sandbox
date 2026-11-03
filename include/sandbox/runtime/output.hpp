#pragma once
#include "sandbox/runtime/bounded_writer.hpp"
#include "sandbox/platform/linux.hpp"
#include "sandbox/model/report.hpp"
namespace sandbox {
class OutputCapture {
  public:
    static constexpr std::size_t queue_capacity = 256 * 1024;
    OutputCapture(UniqueFd source, UniqueFd destination, std::uint64_t limit);
    ~OutputCapture();
    int fd() const {
        return source_.get();
    }
    int readable_fd() const;
    int completion_fd() const {
        return completion_.get();
    }
    void drain();
    StreamReport summary() const;
    bool complete() const;
    void close_incomplete();
    void settle();

  private:
    UniqueFd source_, completion_;
    std::uint64_t limit_, accepted_ = 0, discarded_ = 0, backpressure_ = 0;
    bool eof_ = false;
    std::unique_ptr<BoundedWriter> writer_;
};
} // namespace sandbox
