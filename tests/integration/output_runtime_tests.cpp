#include "sandbox/platform/event_loop.hpp"
#include "sandbox/runtime/output.hpp"
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
using namespace sandbox;
void require(bool value) {
    if (!value)
        throw std::runtime_error("output runtime assertion");
}
int main() {
    try {
        EventLoop loop;
        auto pipe = Pipe::create();
        int old = pipe.read.get();
        require(loop.wait({{old, 1}}, 0).empty());
        pipe.read.reset();
        auto replacement = Pipe::create();
        // Ensure the same integer refers to a new open file description.
        if (replacement.read.get() != old)
            checked(::dup2(replacement.read.get(), old), "reuse descriptor");
        UniqueFd reused(replacement.read.get() == old ? replacement.read.release() : old);
        write_all(replacement.write.get(), "x");
        require(loop.wait({{reused.get(), 2}}, 100).at(0) == 2);
        auto input = Pipe::create();
        OutputCapture capture(std::move(input.read), open_path("/dev/null", O_WRONLY), 1024);
        write_all(input.write.get(), std::string(4096, 'x'));
        input.write.reset();
        auto until = monotonic_ms() + 3000;
        while (!capture.complete()) {
            capture.drain();
            if (monotonic_ms() >= until)
                throw std::runtime_error("writer completion timeout");
            loop.wait({{capture.readable_fd(), 3}, {capture.completion_fd(), 4}}, 20);
        }
        auto summary = capture.summary();
        require(summary.saved == 1024 && summary.discarded == 3072 && summary.flushed);
        auto bad_input = Pipe::create();
        OutputCapture bad(std::move(bad_input.read), open_path("/dev/full", O_WRONLY), 1024);
        write_all(bad_input.write.get(), "error");
        bad_input.write.reset();
        bool failed = false;
        until = monotonic_ms() + 3000;
        while (monotonic_ms() < until) {
            try {
                bad.drain();
            } catch (const std::exception &) {
                failed = true;
                break;
            }
            loop.wait({{bad.readable_fd(), 5}, {bad.completion_fd(), 6}}, 20);
        }
        bad.close_incomplete();
        bad.settle();
        require(failed && !bad.summary().write_error.empty() && !bad.summary().flushed);
        std::cout << "FD reuse, output flush, capture limit and write failure passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
