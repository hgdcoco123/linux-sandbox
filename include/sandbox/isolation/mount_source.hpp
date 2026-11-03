#pragma once
#include "sandbox/platform/linux.hpp"
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace sandbox {
// A pre-clone O_PATH descriptor pins a mount in the parent namespace. It is an
// identity anchor, not a usable MS_BIND source in the child's new mount namespace.
inline UniqueFd reopen_mount_source(int inherited, const fs::path &path) {
    if (!path.is_absolute())
        throw std::runtime_error("mount source must be absolute");
    struct stat expected{}, actual{};
    checked(::fstat(inherited, &expected), "stat inherited mount source");
    if (!S_ISDIR(expected.st_mode))
        throw std::runtime_error("inherited mount source is not a directory");
    open_how how{};
    how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    UniqueFd local(
        checked(static_cast<int>(::syscall(SYS_openat2, AT_FDCWD, path.c_str(), &how, sizeof(how))),
                "reopen mount source in child namespace: " + path.string()));
    checked(::fstat(local.get(), &actual), "stat namespace-local mount source");
    if (actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino ||
        !S_ISDIR(actual.st_mode))
        throw std::runtime_error("mount source identity changed before preparation: " +
                                 path.string());
    return local;
}
} // namespace sandbox
