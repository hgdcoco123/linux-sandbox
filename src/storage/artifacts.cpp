#include "sandbox/interactive.hpp"
#include "sandbox/linux.hpp"
#include "sandbox/sha256.hpp"
#include <algorithm>
#include <array>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sandbox {
namespace {
constexpr std::uint64_t max_bytes = 256ULL * 1024 * 1024;
constexpr std::size_t max_entries = 4096;
struct Budget {
    std::uint64_t bytes = 0;
    std::size_t entries = 0;
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
    if (prefix.size() > 4096 || std::count(prefix.begin(), prefix.end(), '/') > 64)
        throw std::runtime_error("artifact directory depth/path limit exceeded");
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
        if (++budget.entries > max_entries)
            throw std::runtime_error("artifact entry limit exceeded");
        struct stat before{};
        checked(::fstatat(source, name.c_str(), &before, AT_SYMLINK_NOFOLLOW), "stat artifact");
        auto path = prefix + name;
        if (S_ISDIR(before.st_mode)) {
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
        } else if (S_ISREG(before.st_mode)) {
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
                budget.bytes += n;
                digest.update(std::string_view(buf.data(), n));
                if (budget.bytes > max_bytes)
                    throw std::runtime_error("artifact byte limit exceeded");
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
            throw std::runtime_error("symlinks and special files are rejected: " + path);
        errno = 0;
    }
    if (errno)
        system_error("read artifact directory");
    if (dest >= 0)
        checked(::fsync(dest), "fsync artifact directory");
}
} // namespace
Json file_request(const Json &j) {
    require_keys(j, {"op", "source", "destination"}, "files");
    for (auto name : {"source", "destination"})
        if (j.contains(name) && j.at(name).get<std::string>().find('\0') != std::string::npos)
            throw std::runtime_error("NUL in file path");
    auto op = j.at("op").get<std::string>();
    if (op != "snapshot" && op != "manifest" && op != "export")
        throw std::runtime_error("unknown file operation");
    auto source = fs::absolute(j.at("source").get<std::string>()).lexically_normal();
    auto input = directory(source);
    Budget budget;
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
} // namespace sandbox
