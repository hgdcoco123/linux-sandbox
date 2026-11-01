#pragma once
// The /input boundary.
//
// A read-only bind of a host directory is not a boundary: the host can rewrite
// the contents while the target runs, so "the program read the file I put there"
// is not a claim the sandbox can make. InputMode::snapshot_copy captures the tree
// before the launcher starts and points the mount at the copy.
#include "sandbox/config.hpp"

#include <vector>

namespace sandbox {

// Replaces every /input mount with a copy taken now, under `directory`.
//
// The copy reuses the workspace copier, so it inherits the same guarantees:
// ancestors are opened without following symlinks, symlinks/hardlinks/special
// files are refused, files are checked for modification while being read, and the
// destination appears atomically (staging directory + renameat2 NOREPLACE).
//
// Returns the directories it created, in order, so a caller that aborts later can
// undo them. Does nothing for InputMode::bind_readonly.
//
// Throws rather than degrading to a bind: an input that silently stops being a
// snapshot is worse than a run that refuses to start.
[[nodiscard]] std::vector<fs::path> snapshot_inputs(RunConfig &config,
                                                    const fs::path &directory);

} // namespace sandbox
