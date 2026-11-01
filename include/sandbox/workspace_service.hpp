#pragma once
#include "sandbox/core/ops.hpp"
#include "sandbox/core/workspace.hpp"
#include "sandbox/session_store.hpp"

#include <functional>

namespace sandbox {

// The session workspace, as the host sees it.
//
// When the workspace is a quota-enforced tmpfs it exists only inside the sandbox's
// mount namespace, so the host keeps a *published snapshot*: reading it requires
// the sandbox to hand its tree over first, and writing into it requires the
// sandbox to take the tree in. `WorkspaceTransfer` is how this service asks for
// that. An empty transfer means the workspace is a plain host directory (the
// diagnostic backend), which needs neither.
struct WorkspaceTransfer {
    // Refresh the published copy from the sandbox. Throws when the handover fails,
    // because a stale copy must never be presented as the current workspace.
    std::function<void()> publish;
    // Move one staged upload into the sandbox. Called after the upload has been
    // copied into `staging`; throws when the sandbox refuses it.
    std::function<void(const std::string &name)> commit;
    // Where uploads are staged before `commit`. Empty means "straight into the
    // published workspace".
    fs::path staging;
};

class WorkspaceService {
    fs::path workspace_;
    SessionStore &store_;
    WorkspacePolicy policy_;
    WorkspaceTransfer transfer_;

  public:
    WorkspaceService(fs::path workspace, SessionStore &store,
                     WorkspacePolicy policy = WorkspacePolicy{},
                     WorkspaceTransfer transfer = {})
        : workspace_(std::move(workspace)), store_(store), policy_(std::move(policy)),
          transfer_(std::move(transfer)) {}
    Json handle(const Json &request);
    // Typed entry point: the caller decodes the client request once and the
    // service never sees the wire shape again.
    Json handle(const Workspace &operation);
};
} // namespace sandbox
