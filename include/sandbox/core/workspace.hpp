#pragma once
// Workspace boundary: what may be copied in, how much, and what survives.
//
// These are policy questions, so they are answered by a value type instead of
// being scattered as magic numbers in the copy loop. The admission function is
// pure: given what is already stored and one candidate entry, it decides whether
// the entry is acceptable. That makes the whole boundary testable without a
// filesystem, and the copier only has to apply the decision.
#include <cstdint>
#include <optional>
#include <string>

namespace sandbox {

// How the caller's input directory is exposed to the target.
enum class InputMode {
    // Read-only bind of the host directory: cheap, but the host can change the
    // contents while the target runs.
    bind_readonly,
    // Copy taken before the target starts: a real snapshot, at a real cost.
    snapshot_copy,
};

const char *input_mode_name(InputMode) noexcept;

struct WorkspacePolicy {
    std::uint64_t max_bytes = 256ULL * 1024 * 1024;
    std::uint32_t max_entries = 4096;
    std::uint8_t max_depth = 64;
    InputMode input = InputMode::bind_readonly;
    // Keep the workspace after the sandbox closes (debugging) or drop it.
    bool keep_on_close = false;

    // Throws std::invalid_argument for values that cannot describe a useful
    // workspace (zero limits, absurd depth). Called by the config parser so a
    // bad policy never reaches the copier.
    void validate() const;
};

// Why a candidate entry was refused.
enum class Rejection {
    accepted,
    entry_limit,
    byte_limit,
    depth_limit,
    not_regular,
    symlink,
    hardlink,
    escapes_root,
};

const char *rejection_name(Rejection) noexcept;

struct EntryFacts {
    std::uint64_t size = 0;
    std::uint8_t depth = 0;
    bool regular = false;
    bool symlink = false;
    bool hardlink = false;
    // True when the resolved path leaves the source root (for example through a
    // symlinked ancestor).
    bool escapes_root = false;
};

struct Admission {
    Rejection rejection = Rejection::accepted;
    // Always computed, even for a rejection, so callers can report totals.
    std::uint64_t bytes_after = 0;
    std::uint32_t entries_after = 0;

    [[nodiscard]] bool accepted() const noexcept { return rejection == Rejection::accepted; }
};

// Order matters: structural problems (escape, special file) are reported before
// budget problems, because a refused symlink is not an accounting failure.
[[nodiscard]] Admission admit(const WorkspacePolicy &policy, std::uint64_t bytes_used,
                              std::uint32_t entries_used, const EntryFacts &entry) noexcept;

} // namespace sandbox
