#include "sandbox/core/workspace.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace sandbox {

const char *input_mode_name(InputMode mode) noexcept {
    switch (mode) {
    case InputMode::bind_readonly:
        return "bind_readonly";
    case InputMode::snapshot_copy:
        return "snapshot_copy";
    }
    return "unknown";
}

const char *rejection_name(Rejection rejection) noexcept {
    switch (rejection) {
    case Rejection::accepted:
        return "accepted";
    case Rejection::entry_limit:
        return "entry_limit";
    case Rejection::byte_limit:
        return "byte_limit";
    case Rejection::depth_limit:
        return "depth_limit";
    case Rejection::not_regular:
        return "not_regular";
    case Rejection::symlink:
        return "symlink";
    case Rejection::hardlink:
        return "hardlink";
    case Rejection::escapes_root:
        return "escapes_root";
    }
    return "unknown";
}

void WorkspacePolicy::validate() const {
    if (max_bytes == 0)
        throw std::invalid_argument("workspace max_bytes must be positive");
    if (max_entries == 0)
        throw std::invalid_argument("workspace max_entries must be positive");
    if (max_depth == 0)
        throw std::invalid_argument("workspace max_depth must be positive");
    // A snapshot copies every byte into the sandbox; without a byte budget it is
    // unbounded work that still looks like a valid configuration.
    if (input == InputMode::snapshot_copy && max_bytes == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("snapshot_copy requires a finite max_bytes");
}

Admission admit(const WorkspacePolicy &policy, std::uint64_t bytes_used,
                std::uint32_t entries_used, const EntryFacts &entry) noexcept {
    Admission result;
    const std::uint64_t remaining = policy.max_bytes - std::min(bytes_used, policy.max_bytes);
    const bool fits = entry.size <= remaining;
    result.bytes_after = fits ? bytes_used + entry.size : policy.max_bytes;
    const bool room = entries_used < policy.max_entries;
    result.entries_after = room ? entries_used + 1 : policy.max_entries;

    if (entry.escapes_root) {
        result.rejection = Rejection::escapes_root;
    } else if (entry.symlink) {
        result.rejection = Rejection::symlink;
    } else if (entry.hardlink) {
        // A hardlink shares the source inode with files outside the tree.
        result.rejection = Rejection::hardlink;
    } else if (!entry.regular) {
        result.rejection = Rejection::not_regular;
    } else if (entry.depth > policy.max_depth) {
        result.rejection = Rejection::depth_limit;
    } else if (!room) {
        result.rejection = Rejection::entry_limit;
    } else if (!fits) {
        result.rejection = Rejection::byte_limit;
    }
    return result;
}

} // namespace sandbox
