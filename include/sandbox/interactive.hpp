#pragma once
#include "sandbox/config.hpp"
#include "sandbox/core/workspace.hpp"
namespace sandbox {
Json serve_session(RunConfig, const fs::path &state, const fs::path &launcher);
// The workspace boundary travels with the request instead of being rebuilt from
// constants inside the copier.
Json file_request(const Json &, const WorkspacePolicy &);
} // namespace sandbox
