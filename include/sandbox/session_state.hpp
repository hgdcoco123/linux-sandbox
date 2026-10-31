#pragma once
#include "sandbox/session_protocol.hpp"
namespace sandbox {
enum class SessionState { preparing, idle, running, draining, closing, closed, failed };
inline const char *session_state_name(SessionState s) {
    switch (s) {
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
    throw std::logic_error("invalid session state");
}
class SessionLifecycle {
    SessionState state_ = SessionState::preparing;

  public:
    SessionState state() const {
        return state_;
    }
    bool stopping() const {
        return state_ == SessionState::closing || state_ == SessionState::closed ||
               state_ == SessionState::failed;
    }
    void transition(SessionState next) {
        bool legal = (state_ == SessionState::preparing && next == SessionState::idle) ||
                     (state_ == SessionState::idle && next == SessionState::running) ||
                     (state_ == SessionState::running && next == SessionState::draining) ||
                     (state_ == SessionState::draining && next == SessionState::idle) ||
                     (!stopping() && next == SessionState::closing) ||
                     (state_ != SessionState::closed && next == SessionState::failed) ||
                     ((state_ == SessionState::closing || state_ == SessionState::failed) &&
                      next == SessionState::closed);
        if (!legal)
            throw std::logic_error("illegal session transition");
        state_ = next;
    }
    void require_idle() const {
        if (state_ != SessionState::idle)
            throw SessionError(stopping() ? "SESSION_CLOSING" : "SESSION_NOT_IDLE",
                               "operation requires an idle session", !stopping());
    }
    void require_running() const {
        if (state_ != SessionState::running)
            throw SessionError("COMMAND_NOT_ACTIVE",
                               "command does not accept input or cancellation");
    }
};
} // namespace sandbox
