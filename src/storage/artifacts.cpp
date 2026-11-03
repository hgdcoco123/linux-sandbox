#include "sandbox/core/workspace.hpp"
#include "sandbox/storage/artifacts.hpp"
#include "sandbox/platform/linux.hpp"
#include "sandbox/storage/sha256.hpp"
#include "sandbox/storage/snapshot.hpp"
#include <algorithm>
#include <array>
#include <dirent.h>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
namespace sandbox {
namespace {
// Limits live in WorkspacePolicy; this default keeps the historical behaviour
// until the caller supplies a configured policy.
const WorkspacePolicy default_workspace_policy{};
struct Budget {
    const WorkspacePolicy *policy = &default_workspace_policy;
    std::uint64_t bytes = 0;
    std::uint32_t entries = 0;
};
UniqueFd directory(const fs::path &path) {
    // Walk every path component without following symlinks, including ancestors.
    auto abs = fs::absolute(path).lexically_normal();
    UniqueFd fd(checked(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), "open root"));
    for (const auto &part : abs.relative_path()) {
        if (part == "." || part.empty())
            continue;
        fd = UniqueFd(checked(
            ::openat(fd.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
            "open plain directory: " + part.string()));
    }
    return fd;
}
void walk(int source, int dest, const std::string &prefix, Budget &budget, Json &manifest,
          const std::function<bool()> &interrupted = {}) {
    if (prefix.size() > 4096)
        throw std::runtime_error("artifact path limit exceeded");
    DIR *raw =
        ::fdopendir(checked(::fcntl(source, F_DUPFD_CLOEXEC, 3), "duplicate tree directory"));
    if (!raw)
        system_error("fdopendir");
    // A named deleter rather than decltype(&::closedir): the attribute on closedir
    // makes that spelling warn under -Wignored-attributes on GCC 13.
    struct DirectoryCloser {
        void operator()(DIR *handle) const noexcept {
            ::closedir(handle);
        }
    };
    std::unique_ptr<DIR, DirectoryCloser> listing(raw);
    errno = 0;
    while (auto *entry = ::readdir(raw)) {
        // Between entries, so a stop request does not wait behind a whole tree.
        //
        // The callback reads a socket and therefore sets errno (EAGAIN on an empty
        // one). This loop ends by inspecting errno after readdir, so a clobbered
        // errno reads as "the directory failed" on a copy that was fine.
        if (interrupted) {
            const int saved = errno;
            const bool stop = interrupted();
            errno = saved;
            if (stop)
                throw TransferAborted{};
        }
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat before{};
        checked(::fstatat(source, name.c_str(), &before, AT_SYMLINK_NOFOLLOW), "stat artifact");
        auto path = prefix + name;
        // The admission policy is decided in one place (core/workspace) and only
        // applied here; the copier never re-derives limits of its own.
        EntryFacts facts;
        facts.depth = static_cast<std::uint8_t>(
            std::min<std::size_t>(std::count(path.begin(), path.end(), '/'),
                                  std::numeric_limits<std::uint8_t>::max()));
        const bool is_dir = S_ISDIR(before.st_mode);
        const bool is_reg = S_ISREG(before.st_mode);
        facts.symlink = S_ISLNK(before.st_mode);
        facts.hardlink = is_reg && before.st_nlink != 1;
        facts.regular = is_dir || (is_reg && before.st_nlink == 1);
        facts.size = is_reg ? static_cast<std::uint64_t>(before.st_size) : 0;
        const auto admission = admit(*budget.policy, budget.bytes, budget.entries, facts);
        if (!admission.accepted())
            throw std::runtime_error(std::string("artifact refused (") +
                                     rejection_name(admission.rejection) + "): " + path);
        budget.entries = admission.entries_after;
        budget.bytes = admission.bytes_after;
        if (is_dir) {
            UniqueFd child(checked(
                ::openat(source, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                "open artifact directory"));
            UniqueFd target;
            if (dest >= 0) {
                checked(::mkdirat(dest, name.c_str(), 0700), "create artifact directory");
                target = UniqueFd(checked(
                    ::openat(dest, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                    "open destination directory"));
            }
            manifest.push_back({{"path", path}, {"type", "directory"}});
            walk(child.get(), target.get(), path + "/", budget, manifest);
        } else if (is_reg) {
            UniqueFd input(checked(
                ::openat(source, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC),
                "open artifact"));
            struct stat opened{};
            checked(::fstat(input.get(), &opened), "stat opened artifact");
            if (!S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
                opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
                throw std::runtime_error("artifact changed or hardlinked: " + path);
            UniqueFd output;
            if (dest >= 0)
                output = UniqueFd(
                    checked(::openat(dest, name.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600),
                            "create exported file"));
            std::array<char, 65536> buf{};
            std::uint64_t size = 0;
            Sha256 digest;
            for (;;) {
                auto n = ::read(input.get(), buf.data(), buf.size());
                if (n < 0 && errno == EINTR)
                    continue;
                checked(static_cast<int>(n), "read artifact");
                if (!n)
                    break;
                size += n;
                digest.update(std::string_view(buf.data(), n));
                if (output)
                    write_all(output.get(), std::string_view(buf.data(), n));
            }
            struct stat after{};
            checked(::fstat(input.get(), &after), "stat completed artifact");
            if (after.st_size != opened.st_size ||
                size != static_cast<std::uint64_t>(opened.st_size) ||
                after.st_mtim.tv_sec != opened.st_mtim.tv_sec ||
                after.st_mtim.tv_nsec != opened.st_mtim.tv_nsec ||
                after.st_ctim.tv_sec != opened.st_ctim.tv_sec ||
                after.st_ctim.tv_nsec != opened.st_ctim.tv_nsec)
                throw std::runtime_error("artifact modified while reading: " + path);
            if (output)
                checked(::fchmod(output.get(), (opened.st_mode & 0100) ? 0700 : 0600),
                        "preserve owner execute bit");
            if (output)
                checked(::fsync(output.get()), "fsync exported file");
            manifest.push_back(
                {{"path", path}, {"type", "file"}, {"size", size}, {"sha256", digest.hex()}});
        } else
            throw std::runtime_error("artifact refused (not_regular): " + path);
        errno = 0;
    }
    if (errno)
        system_error("read artifact directory");
    if (dest >= 0)
        checked(::fsync(dest), "fsync artifact directory");
}
// A rename is atomic in visibility, not durable: the entry lives in the parent
// directory, so without syncing it a crash can still bring back the old tree. The
// caller decides what an unconfirmed sync means for its report; this only says why.
std::string sync_directory(const fs::path &path) {
    try {
        auto fd = directory(path);
        checked(::fsync(fd.get()), "fsync " + path.string());
        return {};
    } catch (const std::exception &e) {
        return e.what();
    }
}
} // namespace
Json file_request(const Json &j, const WorkspacePolicy &policy) {
    require_keys(j, {"op", "source", "destination"}, "files");
    for (auto name : {"source", "destination"})
        if (j.contains(name) && j.at(name).get<std::string>().find('\0') != std::string::npos)
            throw std::runtime_error("NUL in file path");
    auto op = j.at("op").get<std::string>();
    if (op != "snapshot" && op != "manifest" && op != "export")
        throw std::runtime_error("unknown file operation");
    auto source = fs::absolute(j.at("source").get<std::string>()).lexically_normal();
    auto input = directory(source);
    Budget budget{&policy};
    Json manifest = Json::array();
    if (op == "manifest") {
        walk(input.get(), -1, "", budget, manifest);
        return {{"entries", manifest}, {"bytes", budget.bytes}};
    }
    auto target = fs::absolute(j.at("destination").get<std::string>()).lexically_normal();
    if (target.filename().empty() || target == target.root_path())
        throw std::runtime_error("invalid destination");
    auto parent = directory(target.parent_path());
    // The destination is published only after a complete copy. A failed staging directory is
    // deliberately retained for diagnosis; it never appears as a successful destination.
    auto name = target.filename().string();
    struct stat existing{};
    if (::fstatat(parent.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        throw std::runtime_error("destination already exists");
    if (errno != ENOENT)
        system_error("check destination");
    auto a = source.begin(), b = target.begin();
    while (a != source.end() && b != target.end() && *a == *b) {
        ++a;
        ++b;
    }
    if (a == source.end() || b == target.end())
        throw std::runtime_error("source/destination overlap");
    std::string stage =
        ".sbx-copy-" + std::to_string(::getpid()) + "-" + std::to_string(monotonic_ms());
    checked(::mkdirat(parent.get(), stage.c_str(), 0700), "create staging directory");
    auto output = UniqueFd(checked(
        ::openat(parent.get(), stage.c_str(), O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
        "open staging directory"));
    walk(input.get(), output.get(), "", budget, manifest);
    // renameat2 NOREPLACE avoids replacing a destination created concurrently.
    if (::renameat2(parent.get(), stage.c_str(), parent.get(), name.c_str(), RENAME_NOREPLACE) < 0)
        system_error("publish copy without replacement");
    checked(::fsync(parent.get()), "fsync destination parent");
    return {{"destination", target.string()},
            {"entries", manifest},
            {"bytes", budget.bytes},
            {"complete", true}};
}

// Copy `source` into `dest_parent`/`name`, publishing only once the whole tree
// arrived. Both parents are descriptors, so neither side can name the other's
// filesystem: the host directory is a descriptor init was given, and the workspace
// is a tmpfs only this namespace can see.
Json publish_tree(int source, int dest_parent, const std::string &name,
                  const WorkspacePolicy &policy, const std::function<bool()> &interrupted) {
    struct stat existing{};
    if (::fstatat(dest_parent, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        throw std::runtime_error("destination already exists");
    if (errno != ENOENT)
        system_error("check destination");
    std::string stage =
        ".sbx-copy-" + std::to_string(::getpid()) + "-" + std::to_string(monotonic_ms());
    checked(::mkdirat(dest_parent, stage.c_str(), 0700), "create staging directory");
    auto output = UniqueFd(checked(
        ::openat(dest_parent, stage.c_str(), O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
        "open staging directory"));
    Budget budget{&policy};
    Json manifest = Json::array();
    walk(source, output.get(), "", budget, manifest, interrupted);
    if (::renameat2(dest_parent, stage.c_str(), dest_parent, name.c_str(), RENAME_NOREPLACE) < 0)
        throw std::runtime_error("publish copy without replacement: " + name + ": " +
                                 std::strerror(errno));
    checked(::fsync(dest_parent), "fsync destination parent");
    return {{"entries", manifest}, {"bytes", budget.bytes}, {"complete", true}};
}

// Publish `incoming` over `published` in one atomic step. Exchanging first is what
// keeps the previous copy intact until the new tree is actually in place; removing the
// old copy first would lose it whenever the publish failed or the process died in
// between.
//
// The two paths that publish (a single run's export and a session handover) share this
// one implementation: a publish rule that exists twice drifts, and then two callers
// disagree about what "delivered" means.
//
// Everything after the exchange is a returned fact, not an exception: the artifacts are
// visible at that point, so failing the call would misreport them. See PublishResult.
PublishResult publish_tree_over(const fs::path &incoming, const fs::path &published,
                                const WorkspacePolicy &policy) {
    PublishResult result;
    if (::renameat2(AT_FDCWD, published.c_str(), AT_FDCWD, incoming.c_str(),
                    RENAME_EXCHANGE) == 0) {
        // The swap is visible now. Confirm it on stable storage before touching the old
        // copy: if the sync fails, that copy is the only version known to be on disk and
        // reclaiming it could leave a crash with neither.
        result.published = true;
        result.durability_error = sync_directory(published.parent_path());
        if (result.durability_error.empty())
            result.durability_error = sync_directory(incoming.parent_path());
        result.durability_confirmed = result.durability_error.empty();
        if (!result.durability_confirmed) {
            // The recovery point is where the exchange left the previous copy.
            result.retained_previous_path = incoming;
            return result;
        }
        // The previous copy now sits where the export landed: reclaiming it is cleanup.
        std::error_code reclaimed;
        fs::remove_all(incoming, reclaimed);
        if (reclaimed)
            result.cleanup_error = "previous copy could not be reclaimed: " + reclaimed.message();
        return result;
    }
    const int error = errno;
    if (error != EXDEV)
        throw std::runtime_error("publish: " + std::string(std::strerror(error)));
    // An exchange needs both paths on one filesystem, and the run directory need not
    // share one with the destination. Copy next to the destination first, then exchange
    // *within* its filesystem: that keeps both properties - atomic visibility, and the
    // old copy surviving until the swap.
    auto parent = published.parent_path();
    auto source = open_path(incoming, O_RDONLY | O_DIRECTORY);
    auto destination = open_path(parent, O_RDONLY | O_DIRECTORY);
    const auto staging =
        ".sbx-publish-" + std::to_string(::getpid()) + "-" + std::to_string(monotonic_ms());
    publish_tree(source.get(), destination.get(), staging, policy);
    if (::renameat2(AT_FDCWD, (parent / staging).c_str(), AT_FDCWD, published.c_str(),
                    RENAME_EXCHANGE) < 0) {
        const int failed = errno;
        std::error_code ignored;
        fs::remove_all(parent / staging, ignored);
        throw std::runtime_error("publish: " + std::string(std::strerror(failed)));
    }
    result.published = true;
    result.durability_error = sync_directory(parent);
    result.durability_confirmed = result.durability_error.empty();
    if (!result.durability_confirmed) {
        // The generated staging name is exactly why the path has to be reported: the
        // caller cannot reconstruct it from the tag or the destination.
        result.retained_previous_path = parent / staging;
        return result;
    }
    // The exchange put the previous copy where the staging directory was.
    std::error_code reclaimed;
    fs::remove_all(parent / staging, reclaimed);
    if (reclaimed)
        result.cleanup_error = "previous copy could not be reclaimed: " + reclaimed.message();
    return result;
}

void create_transfer_layout(const fs::path &run_directory) {
    create_directories_or_remove({run_directory / "transfer", run_directory / "transfer" / "in"});
}

Json export_workspace(int transfer, const std::string &tag, const WorkspacePolicy &policy,
                      const std::function<bool()> &interrupted) {
    auto workspace = directory("/output");
    return publish_tree(workspace.get(), transfer, tag, policy, interrupted);
}

Json import_workspace(int transfer, const std::string &name, const WorkspacePolicy &policy,
                      const std::function<bool()> &interrupted) {
    // Uploads are staged under `in/` so a name can never collide with an export tag.
    UniqueFd inbox(checked(::openat(transfer, "in", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                           "open transfer inbox"));
    UniqueFd source(checked(
        ::openat(inbox.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
        "open staged import tree: " + name));
    auto output = directory("/output");
    return publish_tree(source.get(), output.get(), name, policy, interrupted);
}
std::vector<fs::path> snapshot_inputs(RunConfig &config, const fs::path &directory) {
    std::vector<fs::path> created;
    if (config.workspace.input != InputMode::snapshot_copy)
        return created;
    for (auto &mount : config.mounts) {
        if (mount.target != "/input")
            continue;
        auto destination = directory / "input";
        // Only one /input mount is meaningful; two would silently shadow each other.
        if (!created.empty())
            throw std::runtime_error("only one /input mount can be snapshotted");
        // Reusing the copier inherits its guarantees: ancestors opened without
        // following symlinks, symlinks/hardlinks/special files refused, files
        // re-checked while being read, destination published atomically.
        file_request({{"op", "snapshot"},
                      {"source", mount.source.string()},
                      {"destination", destination.string()}},
                     config.workspace);
        created.push_back(destination);
        // From here on the sandbox mounts the copy; the host directory is no
        // longer referenced by anything downstream.
        mount.source = destination;
    }
    return created;
}
} // namespace sandbox
