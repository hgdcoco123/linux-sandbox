#pragma once
#include "sandbox/core/record.hpp"
#include "sandbox/core/supervisor.hpp"
#include "sandbox/output.hpp"
#include <fcntl.h>
#include <memory>
namespace sandbox {
// One session command in flight.
//
// Preparing one is off-session: a constructor failure releases every descriptor
// and capture, so the session remains idle until this whole bundle is committed.
//
// The command is supervised by the shared ExecutionCore, exactly like a single
// run. The session differs only in who owns the process group: init does, and it
// reports the group empty as a fact, so the session never drives a teardown of
// its own for a command that finished on its own.
struct CommandExecution {
    Pipe input = Pipe::create(), output = Pipe::create(), error = Pipe::create();
    std::unique_ptr<OutputCapture> out, err;
    std::unique_ptr<ExecutionCore> core;
    // The durable intent, and the source of the live summary the wire reports.
    std::optional<AcceptedCommand> accepted;
    Json memory_peak = nullptr;
    std::uint64_t started = 0, wall_ms = 0;
    // init reported this command finished and its descendant set empty.
    bool done = false;
    // Output facts are latched so the core sees each transition once.
    bool out_eof_seen = false, out_done_seen = false;
    bool err_eof_seen = false, err_done_seen = false;
    CommandExecution(const fs::path &directory, std::uint64_t capture_bytes) {
        nonblocking(input.write.get());
        out = std::make_unique<OutputCapture>(
            std::move(output.read),
            open_path(directory / "stdout.bin", O_WRONLY | O_CREAT | O_EXCL), capture_bytes);
        err = std::make_unique<OutputCapture>(
            std::move(error.read),
            open_path(directory / "stderr.bin", O_WRONLY | O_CREAT | O_EXCL), capture_bytes);
    }
    void drain() {
        out->drain();
        err->drain();
    }
    void release_child_ends() {
        input.read.reset();
        output.write.reset();
        error.write.reset();
    }
};
} // namespace sandbox
