#pragma once
// Steps shared by both pid-1 modes (one-shot and persistent).
//
// The two modes differ in one thing only: how many commands this init will run.
// Everything that is true of a pid-1 regardless — tying itself to the supervisor's
// lifetime, adopting orphans, building the environment, dropping capabilities, and
// supervising one target process — lives here or in init.cpp, once.
#include "sandbox/model/config.hpp"
#include "sandbox/runtime/protocol.hpp"

namespace sandbox {

// What the target's child side writes into the exec-error pipe when it cannot
// reach execve. The stage says where it stopped, because "exec failed" without
// that is not diagnosable.
struct ExecFailure {
    int stage;
    int error;
};

// Stages reported through ExecFailure::stage.
enum : int {
    exec_stage_pdeathsig = 1,
    exec_stage_setpgid = 2,
    exec_stage_chdir = 3,
    exec_stage_descriptors = 4,
    exec_stage_security = 5,
    exec_stage_execve = 6,
};

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
