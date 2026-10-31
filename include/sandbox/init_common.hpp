#pragma once
// Steps shared by both pid-1 implementations (one-shot and persistent).
//
// Only the environment half lives here: the command loop differs by design, but
// "how the namespace and credentials are brought up" must not be written twice,
// because the two copies drifted apart once already.
#include "sandbox/config.hpp"
#include "sandbox/protocol.hpp"

namespace sandbox {

// Fast, side-effect-free steps that must happen before any message is awaited:
// tie init to the supervisor's lifetime, and adopt orphans in the diagnostic
// backend (the isolated backend is already a pid namespace).
void arm_init(bool isolated);

// Filesystem and credential preparation, after the supervisor says PREPARE:
//   1. build the root filesystem, or drop the source-directory descriptor
//   2. shrink capabilities, then make init undumpable
//   3. re-arm parent death, which the credential changes can clear
//
// Throws on failure; the caller reports INIT_ERROR and exits non-zero, because a
// half-prepared environment must never be used.
void prepare_environment(const RunConfig &config, const fs::path &root);

} // namespace sandbox
