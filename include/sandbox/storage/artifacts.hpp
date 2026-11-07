#pragma once
#include "sandbox/model/config.hpp"
#include "sandbox/core/workspace.hpp"

#include <functional>
namespace sandbox {
// The workspace boundary travels with the request instead of being rebuilt from
// constants inside the copier.
Json file_request(const Json &, const WorkspacePolicy &);

// Workspace transfer across the namespace boundary, descriptor to descriptor.
//
// The sandbox workspace is a tmpfs the host cannot see, and the host directory is
// something the sandbox must not be able to name. Both directions therefore work on
// descriptors: `transfer` is the host directory as a descriptor the supervisor
// passed once, and the sandbox side is opened by path because init owns that
// namespace. Nothing about the copy guarantees changes - it is the same walker that
// rejects symlinks, hardlinks and special files and re-checks every file.
// Copy `source` into `dest_parent`/`name` and publish it only once the whole tree
// arrived. Exposed for tests: it is the part of the transfer that does not need a
// namespace, so it can be checked without a delegated cgroup.
// A copy that stopped because the supervisor asked the sandbox to stop. It is not a
// failure of the copy: it means the copy really did stop, which is why it must not be
// treated like a timeout (those leave the copy running and poison the channel).
struct TransferAborted : std::runtime_error {
    TransferAborted() : std::runtime_error("transfer interrupted by a stop request") {}
};

// `interrupted` is polled between entries; returning true aborts the copy and leaves
// nothing published. Empty means "never interrupt".
Json publish_tree(int source, int dest_parent, const std::string &name,
                  const WorkspacePolicy &, const std::function<bool()> &interrupted = {});
// Publishing is three facts, not one: the new tree is the visible one, that visibility
// is on stable storage, and the previous copy has been reclaimed. Returning a single
// string merged the last two into "cleanup", which hid "the artifacts are visible but a
// crash may still bring back the old ones" behind a cleanup warning.
//
// A failure before the exchange throws (nothing was published). Everything after it is
// reported here, because the artifacts are already in place at that point.
struct PublishResult {
    bool published = false;
    bool durability_confirmed = false;
    std::string durability_error;
    std::string cleanup_error;
    // Where the previous copy was left when durability could not be confirmed, so the
    // fact is actionable rather than just a warning. Empty when nothing was retained.
    // Matters most on the cross-filesystem path: the staging directory there has a
    // generated name, so a caller could not find the recovery point on its own.
    fs::path retained_previous_path;
};
// Create run_directory/transfer and its inbox as one transaction: either both exist on
// return, or nothing this call created is left behind. Shared by the single-run path so
// the failure behaviour lives (and is tested) in one place.
void create_transfer_layout(const fs::path &run_directory);
// Publish `incoming` over `published` atomically (exchange, with a copy-then-exchange
// fallback across filesystems). After the swap the directories are synced; **if that
// sync fails the previous copy is left in place** instead of being reclaimed - it is the
// only version known to be on disk, so a crash must not end up with neither. Shared by
// the single-run export and the session handover.
PublishResult publish_tree_over(const fs::path &incoming, const fs::path &published,
                                const WorkspacePolicy &);
// A manifest of what was delivered: path, type, size and sha256 per entry, so a consumer
// can prove the tree it received is the tree the sandbox published. Bounded by an entry
// count and a hashing budget; `truncated` says when the list or the digests stopped short.
Json manifest_of(const fs::path &root, std::size_t max_entries, std::uint64_t max_hash_bytes);
Json export_workspace(int transfer, const std::string &tag, const WorkspacePolicy &,
                      const std::function<bool()> &interrupted = {});
Json import_workspace(int transfer, const std::string &name, const WorkspacePolicy &,
                      const std::function<bool()> &interrupted = {});
} // namespace sandbox
