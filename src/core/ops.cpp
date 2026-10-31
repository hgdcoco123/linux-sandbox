#include "sandbox/core/ops.hpp"

namespace sandbox {
namespace {
[[nodiscard]] bool is_absolute_path(const std::string &text) {
    return !text.empty() && text.front() == '/';
}
} // namespace

const char *stream_name(Stream stream) noexcept {
    switch (stream) {
    case Stream::standard_output:
        return "stdout";
    case Stream::standard_error:
        return "stderr";
    }
    return "unknown";
}

const char *session_state_name(SessionState state) noexcept {
    switch (state) {
    case SessionState::created:
        return "created";
    case SessionState::preparing:
        return "preparing";
    case SessionState::idle:
        return "idle";
    case SessionState::running:
        return "running";
    case SessionState::draining:
        return "draining";
    case SessionState::closing:
        return "closing";
    case SessionState::closed:
        return "closed";
    case SessionState::failed:
        return "failed";
    }
    return "unknown";
}

SessionRequest SessionRequest::create(SessionOperation operation) {
    std::visit(
        [](auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, StdinWrite>) {
                if (value.bytes.size() > max_stdin_chunk)
                    throw RequestError("INVALID_REQUEST", "stdin chunk exceeds 4096 bytes");
            } else if constexpr (std::is_same_v<T, Workspace>) {
                // The nested operation carries its own invariants.
                std::visit(
                    [](auto &workspace) {
                        using W = std::decay_t<decltype(workspace)>;
                        if constexpr (std::is_same_v<W, WorkspaceSnapshot>) {
                            if (!is_absolute_path(workspace.source))
                                throw RequestError("INVALID_REQUEST",
                                                   "snapshot source must be absolute");
                            if (workspace.name.empty() || workspace.name.size() > 128)
                                throw RequestError("INVALID_REQUEST",
                                                   "snapshot name must be 1..128 characters");
                        } else if constexpr (std::is_same_v<W, WorkspaceExport>) {
                            if (!is_absolute_path(workspace.destination))
                                throw RequestError("INVALID_REQUEST",
                                                   "export destination must be absolute");
                        }
                    },
                    value.operation);
            }
        },
        operation);
    return SessionRequest(std::move(operation));
}

} // namespace sandbox
