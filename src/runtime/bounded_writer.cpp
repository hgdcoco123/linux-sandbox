#include "sandbox/bounded_writer.hpp"
#include <algorithm>
#include <stdexcept>
namespace sandbox {
BoundedWriter::BoundedWriter(std::unique_ptr<OutputSink> sink, std::size_t capacity,
                             std::function<void()> notify)
    : sink_(std::move(sink)), capacity_(capacity), notify_(std::move(notify)) {
    if (!sink_ || !capacity_)
        throw std::invalid_argument("writer requires a sink and positive capacity");
    worker_ = std::thread([this] { work(); });
}
BoundedWriter::~BoundedWriter() {
    close();
    join();
}
void BoundedWriter::join() {
    if (worker_.joinable())
        worker_.join();
}
void BoundedWriter::close() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    ready_.notify_one();
}
std::size_t BoundedWriter::available() const {
    std::lock_guard lock(mutex_);
    return closing_ || status_.finished ? 0 : capacity_ - status_.outstanding;
}
WriterStatus BoundedWriter::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}
bool BoundedWriter::try_submit(std::vector<char> &&block) {
    if (block.empty())
        return true;
    {
        std::lock_guard lock(mutex_);
        if (closing_ || status_.finished || block.size() > capacity_ - status_.outstanding)
            return false;
        auto size = block.size();
        queue_.push_back(std::move(block)); // allocation failure leaves accounting unchanged
        status_.outstanding += size;
        status_.peak = std::max(status_.peak, status_.outstanding);
    }
    ready_.notify_one();
    return true;
}
void BoundedWriter::notify() noexcept {
    try {
        if (notify_)
            notify_();
    } catch (...) {
    }
}
void BoundedWriter::work() noexcept {
    try {
        for (;;) {
            std::vector<char> block;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                if (queue_.empty()) {
                    status_.finished = true;
                    lock.unlock();
                    notify();
                    return;
                }
                block = std::move(queue_.front());
                queue_.pop_front();
            }
            std::size_t offset = 0;
            while (offset < block.size()) {
                auto n = sink_->write(std::span<const char>(block).subspan(offset));
                if (!n || n > block.size() - offset)
                    throw std::runtime_error("output sink made invalid progress");
                offset += n;
                std::lock_guard lock(mutex_);
                status_.written += n;
            }
            {
                std::lock_guard lock(mutex_);
                status_.outstanding -= block.size();
            }
            notify();
        }
    } catch (const std::exception &e) {
        std::lock_guard lock(mutex_);
        status_.error = e.what();
        status_.finished = true;
        closing_ = true;
        queue_.clear();
        status_.outstanding = 0;
    } catch (...) {
        std::lock_guard lock(mutex_);
        status_.error = "unknown output writer failure";
        status_.finished = true;
        closing_ = true;
        queue_.clear();
        status_.outstanding = 0;
    }
    notify();
}
} // namespace sandbox
