#include "sandbox/isolation/isolation.hpp"
#include "sandbox/isolation/mount_source.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/mount.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace sandbox {
namespace {
void mount_call(const char *source, const fs::path &target, const char *type, unsigned long flags,
                const char *data = nullptr) {
    checked(::mount(source, target.c_str(), type, flags, data),
            "mount source=" + std::string(source ? source : "(none)") +
                " target=" + target.string() + " flags=" + std::to_string(flags));
}
void attributes(const fs::path &target, bool readonly, bool noexec) {
    struct mount_attr a{};
    a.attr_set = MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV;
    if (readonly)
        a.attr_set |= MOUNT_ATTR_RDONLY;
    if (noexec)
        a.attr_set |= MOUNT_ATTR_NOEXEC;
    else
        a.attr_clr |= MOUNT_ATTR_NOEXEC;
    checked(static_cast<int>(::syscall(SYS_mount_setattr, AT_FDCWD, target.c_str(), AT_RECURSIVE,
                                       &a, sizeof(a))),
            "mount_setattr " + target.string());
}
} // namespace
void RootfsBuilder::build(const RunConfig &c, const fs::path &root) {
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
        auto error = errno;
        throw std::runtime_error(
            "isolation environment initialization: make_mounts_private failed (errno=" +
            std::to_string(error) + ", " + std::strerror(error) +
            "). This operation requires mount capabilities in the new user namespace. Check "
            "sandbox doctor: AppArmor/LSM or inherited confinement may deny it. No automatic "
            "privilege escalation or unsafe fallback is performed.");
    }
    // Inherited source FDs still reference the parent's mount objects. Reopen in
    // this namespace, rejecting symlinks and checking the pinned inode identity.
    // Every mount gets a descriptor whether or not it is bound: the numbers were
    // handed over by the launcher in mount order, so skipping one would shift all
    // the ones after it onto the wrong source.
    const bool quota_output = !c.workspace_transfer.empty();
    std::vector<UniqueFd> local_sources;
    local_sources.push_back(reopen_mount_source(6, c.runtime_root));
    for (std::size_t i = 0; i < c.mounts.size(); ++i)
        local_sources.push_back(reopen_mount_source(static_cast<int>(7 + i), c.mounts[i].source));
    auto runtime_source = "/proc/self/fd/" + std::to_string(local_sources[0].get());
    mount_call(runtime_source.c_str(), root, nullptr, MS_BIND);
    attributes(root, true, false);
    for (std::size_t i = 0; i < c.mounts.size(); ++i) {
        // With a quota workspace the caller's /output directory is the export
        // destination, not the workspace: binding it here would either be hidden by
        // the tmpfs mounted below (leaving the caller's directory empty) or, if the
        // order ever changed, silently remove the quota.
        if (quota_output && c.mounts[i].target == "/output")
            continue;
        auto target = root / c.mounts[i].target.substr(1);
        auto source = "/proc/self/fd/" + std::to_string(local_sources[i + 1].get());
        mount_call(source.c_str(), target, nullptr, MS_BIND);
        // Only the managed persistent workspace permits executing newly built programs.
        attributes(target, c.mounts[i].readonly,
                   !(c.persistent && c.mounts[i].target == "/output"));
    }
    if (!c.workspace_transfer.empty()) {
        // The workspace is a tmpfs owned by this namespace. The supervisor cannot
        // see it, so these two numbers are the only thing bounding what the target
        // can write: bytes and inodes, enforced by the kernel, cumulative over the
        // whole session. Exec is allowed because a session builds and runs its own
        // artifacts inside the workspace.
        auto quota = "size=" + std::to_string(c.workspace.max_bytes) +
                     ",nr_inodes=" + std::to_string(c.workspace.max_entries) + ",mode=0700";
        mount_call("tmpfs", root / "output", "tmpfs", MS_NOSUID | MS_NODEV, quota.c_str());
        // The supervisor cannot see this tmpfs, so data crosses the boundary through
        // a descriptor it hands over for the duration of the run (see transfer_*).
    }
    auto size = "size=" + std::to_string(c.limits.tmp_bytes) + ",mode=0700";
    for (auto name : {"work", "tmp"})
        mount_call("tmpfs", root / name, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, size.c_str());
    // Minimal devices are bind-mounted before old root is detached. No block devices.
    mount_call("tmpfs", root / "dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "size=64k,mode=0755");
    for (auto device : {"null", "zero", "random", "urandom"}) {
        auto dest = root / "dev" / device;
        auto f = open_path(dest, O_CREAT | O_WRONLY | O_EXCL);
        auto source = "/dev/" + std::string(device);
        mount_call(source.c_str(), dest, nullptr,
                   MS_BIND); // device mounts intentionally do not use nodev
    }
    // Mount the new PID namespace's procfs while the inherited proc mount is
    // still visible. Detaching the old tree first can trigger the kernel's
    // mount_too_revealing check and reject a fresh procfs with EPERM.
    // This runs in P2, so procfs describes the sandbox PID namespace, not P1's.
    mount_call("proc", root / "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC);
    checked(::chdir(root.c_str()), "chdir new root");
    checked(static_cast<int>(::syscall(SYS_pivot_root, ".", ".oldroot")), "pivot_root");
    checked(::chdir("/"), "chdir /");
    checked(::umount2("/.oldroot", MNT_DETACH), "detach old root");
    for (std::size_t i = 0; i < c.mounts.size() + 1; ++i)
        ::close(static_cast<int>(6 + i));
}
} // namespace sandbox
