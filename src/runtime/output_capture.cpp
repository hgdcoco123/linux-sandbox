#include "sandbox/runtime/output.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/eventfd.h>
#include <unistd.h>
namespace sandbox {
namespace {
class FileOutputSink final : public OutputSink {
    UniqueFd destination_;

  public:
    explicit FileOutputSink(UniqueFd fd) : destination_(std::move(fd)) {}
    std::size_t write(std::span<const char> bytes) override {
        ssize_t n;
        do {
            n = ::write(destination_.get(), bytes.data(), bytes.size());
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            system_error("write captured output");
        return static_cast<std::size_t>(n);
    }
};
} // namespace
OutputCapture::OutputCapture(UniqueFd s, UniqueFd d, std::uint64_t limit)
    : source_(std::move(s)),
      completion_(checked(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), "output eventfd")),
      limit_(limit) {
    nonblocking(source_.get());
    writer_ = std::make_unique<BoundedWriter>(
        std::make_unique<FileOutputSink>(std::move(d)), queue_capacity, [this] {
            std::uint64_t one = 1;
            while (::write(completion_.get(), &one, sizeof(one)) < 0 && errno == EINTR) {
            }
            // EAGAIN means a wakeup is already pending. Lifetime is protected by join.
        });
}
OutputCapture::~OutputCapture() {
    source_.reset();
    if (writer_) {
        writer_->close();
        writer_->join();
    }
}
int OutputCapture::readable_fd() const {
    return accepted_ < limit_ && writer_->available() == 0 ? -1 : source_.get();
}
StreamReport OutputCapture::summary() const {
    auto status = writer_->status();
    StreamReport report;
    report.saved = status.written;
    report.discarded = discarded_;
    report.eof = eof_;
    report.flushed = status.finished && status.error.empty();
    report.queue_peak_bytes = status.peak;
    report.backpressure_events = backpressure_;
    report.write_error = status.error;
    report.failed_bytes = status.error.empty() ? 0 : accepted_ - status.written;
    return report;
}
bool OutputCapture::complete() const {
    auto s = summary();
    return s.eof && s.flushed;
}
void OutputCapture::close_incomplete() {
    // Abandoning the producer ends production for our purposes: without this the
    // stream never reports eof after a forced drain, the supervisor waits for a
    // fact that cannot arrive, and the run only ends at the cleanup deadline.
    eof_ = true;
    source_.reset();
    writer_->close();
}
void OutputCapture::settle() {
    writer_->close();
    writer_->join();
}
void OutputCapture::drain() {
    std::uint64_t notifications;
    while (::read(completion_.get(), &notifications, sizeof(notifications)) > 0) {
    }
    auto status = writer_->status();
    if (!status.error.empty())
        throw std::runtime_error("output writer: " + status.error);
    std::array<char, 16384> data;
    std::size_t budget = 65536;
    while (source_ && budget) {
        auto count = std::min(data.size(), budget);
        if (accepted_ < limit_) {
            count = std::min<std::uint64_t>(count, limit_ - accepted_);
            count = std::min(count, writer_->available());
            if (!count) {
                ++backpressure_;
                return;
            }
        }
        auto n = ::read(source_.get(), data.data(), count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n < 0)
            system_error("read target output");
        if (!n) {
            source_.reset();
            eof_ = true;
            writer_->close();
            return;
        }
        auto bytes = static_cast<std::size_t>(n);
        if (accepted_ < limit_) {
            std::vector<char> block(data.begin(), data.begin() + n);
            if (!writer_->try_submit(std::move(block))) {
                // Single producer: capacity cannot shrink between available and submit.
                // Rejection means the worker failed; account for bytes consumed from pipe.
                accepted_ += bytes;
                throw std::runtime_error("output writer stopped accepting data");
            }
            accepted_ += bytes;
        } else
            discarded_ += bytes;
        budget -= bytes;
    }
}
} // namespace sandbox
