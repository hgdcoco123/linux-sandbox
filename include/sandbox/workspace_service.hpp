#pragma once
#include "sandbox/core/ops.hpp"
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
    // Typed entry point: the caller decodes the client request once and the
    // service never sees the wire shape again.
    Json handle(const Workspace &operation);
};
} // namespace sandbox
