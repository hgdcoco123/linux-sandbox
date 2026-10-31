#pragma once
#include "sandbox/command_model.hpp"
#include "sandbox/output.hpp"
#include <fcntl.h>
#include <memory>
namespace sandbox {
// Prepare off-session: a constructor failure releases every descriptor and capture.
// The caller's session remains idle until this complete bundle is committed.
struct CommandExecution {
    Pipe input = Pipe::create(), output = Pipe::create(), error = Pipe::create();
    std::unique_ptr<OutputCapture> out, err;
    std::optional<CommandRecord> record;
    Json baseline = nullptr, memory_peak = nullptr;
    bool drained() const {
        return done && out->complete() && err->complete();
    }
    void drain() {
        out->drain();
        err->drain();
    }
    void finish(Json metrics) {
        if (!drained())
            throw std::logic_error("command output is not drained");
        record->finish(out->summary(), err->summary(), std::move(metrics));
    }
    std::uint64_t started = 0, wall_ms = 0;
    bool done = false;
    void release_child_ends() {
        input.read.reset();
        output.write.reset();
        error.write.reset();
    }
    CommandExecution(const fs::path &directory, std::uint64_t capture_bytes) {
        nonblocking(input.write.get());
        out = std::make_unique<OutputCapture>(
            std::move(output.read),
            open_path(directory / "stdout.bin", O_WRONLY | O_CREAT | O_EXCL), capture_bytes);
        err = std::make_unique<OutputCapture>(
            std::move(error.read), open_path(directory / "stderr.bin", O_WRONLY | O_CREAT | O_EXCL),
            capture_bytes);
    }
};
} // namespace sandbox
