#include "sandbox/core/workspace.hpp"
#include "sandbox/interactive.hpp"
#include "sandbox/linux.hpp"
#include "sandbox/sha256.hpp"
#include "sandbox/snapshot.hpp"
#include <algorithm>
#include <array>
#include <dirent.h>
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
            "open plain directory"));
    }
    return fd;
}
void walk(int source, int dest, const std::string &prefix, Budget &budget, Json &manifest) {
    if (prefix.size() > 4096)
        throw std::runtime_error("artifact path limit exceeded");
    DIR *raw =
        ::fdopendir(checked(::fcntl(source, F_DUPFD_CLOEXEC, 3), "duplicate tree directory"));
    if (!raw)
        system_error("fdopendir");
    std::unique_ptr<DIR, decltype(&::closedir)> listing(raw, ::closedir);
    errno = 0;
    while (auto *entry = ::readdir(raw)) {
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
