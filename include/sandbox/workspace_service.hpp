#pragma once
#include "sandbox/session_store.hpp"
namespace sandbox {
class WorkspaceService {
    fs::path workspace_;
    SessionStore &store_;

  public:
    WorkspaceService(fs::path workspace, SessionStore &store)
        : workspace_(std::move(workspace)), store_(store) {}
    Json handle(const Json &request);
};
} // namespace sandbox
