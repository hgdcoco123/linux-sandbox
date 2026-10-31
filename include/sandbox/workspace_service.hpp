#pragma once
#include "sandbox/core/workspace.hpp"
#include "sandbox/session_store.hpp"
namespace sandbox {
class WorkspaceService {
    fs::path workspace_;
    SessionStore &store_;
    WorkspacePolicy policy_;

  public:
    WorkspaceService(fs::path workspace, SessionStore &store,
                     WorkspacePolicy policy = WorkspacePolicy{})
        : workspace_(std::move(workspace)), store_(store), policy_(policy) {}
    Json handle(const Json &request);
};
} // namespace sandbox
