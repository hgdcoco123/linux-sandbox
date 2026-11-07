#include "sandbox/application/service.hpp"
#include "sandbox/platform/environment.hpp"
#include <cerrno>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
namespace sandbox {
namespace {
// The probe can only report where isolation stopped. Turning that into "what do I type next"
// here means a user never has to guess between the sysctl, the AppArmor profile and the
// delegated-cgroup route.
std::string probe_remedy(const Json &probe, const Json &policy) {
    const auto text = [](const Json &value, const char *key) -> std::string {
        if (!value.contains(key))
            return {};
        if (value[key].is_string())
            return value[key].get<std::string>();
        if (value[key].is_number())
            return std::to_string(value[key].get<std::int64_t>());
        return {};
    };
    const auto stage = text(probe, "stage");
    const auto number = text(policy, "apparmor_restrict_unprivileged_userns");
    const auto clone = text(policy, "unprivileged_userns_clone");
    const bool restricted = number.rfind("1", 0) == 0;
    const bool clone_off = clone.rfind("0", 0) == 0;
    if (stage == "make_mounts_private" && restricted)
        return "kernel.apparmor_restrict_unprivileged_userns=1 blocks unprivileged user "
               "namespaces. Temporarily: sudo sysctl -w "
               "kernel.apparmor_restrict_unprivileged_userns=0 (restore it afterwards). "
               "Permanently: allow it for this launcher with an AppArmor profile that grants "
               "`userns,` plus the launcher path. See docs/INSTALL.md section 3.0.";
    if (stage == "make_mounts_private" && clone_off)
        return "kernel.unprivileged_userns_clone=0 disables user namespaces. Try: sudo sysctl -w "
               "kernel.unprivileged_userns_clone=1. See docs/INSTALL.md section 3.0.";
    if (stage == "make_mounts_private")
        return "mount namespaces cannot be made private in this environment (container without "
               "CAP_SYS_ADMIN, or a restrictive LSM). Use the diagnostic backend (--process) for "
               "the pipeline, or run on a host that allows unprivileged user namespaces. See "
               "docs/INSTALL.md section 3.0.";
    if (stage == "create_user_mount_namespace")
        return "creating a user namespace failed. Check /proc/sys/user/max_user_namespaces and "
               "any LSM denial in dmesg; run `sandbox doctor` inside "
               "`systemd-run --user -p Delegate=yes` to separate the two problems (docs/INSTALL.md 3.0).";
    if (stage == "cgroup")
        return "no delegated cgroup subtree. Run inside `systemd-run --user -p Delegate=yes`, or "
               "prepare one with tools/cgroup_setup.py and pass --cgroup-root. See "
               "docs/INSTALL.md section 3.1.";
    if (stage == "probe_process")
        return "the probe process itself failed; this is a harness problem, not a host policy. "
               "Attach the full `sandbox doctor` output to an issue.";
    return "isolation is unavailable for an unclassified reason; `sandbox doctor` reports the "
           "stage, docs/INSTALL.md section 3.0 maps stages to fixes.";
}
bool prefix(const fs::path &a, const fs::path &b) {
    auto i = a.begin(), j = b.begin();
    for (; j != b.end(); ++j, ++i)
        if (i == a.end() || *i != *j)
            return false;
    return true;
}
void validate_state_separation(const RunConfig &c, const fs::path &state) {
    auto root = fs::weakly_canonical(fs::absolute(state));
    for (auto &m : c.mounts) {
        auto p = fs::canonical(m.source);
        if (prefix(root, p) || prefix(p, root))
            throw std::runtime_error("state root overlaps an exposed directory");
    }
    if (!c.runtime_root.empty()) {
        auto p = fs::canonical(c.runtime_root);
        if (prefix(root, p) || prefix(p, root))
            throw std::runtime_error("state root overlaps runtime");
    }
}
} // namespace
RunReport SandboxService::run(const fs::path &file, bool allow, bool quiet) {
    auto c = RunConfig::load(file);
    c.validate(allow);
    c.quiet = quiet;
    if (c.persistent)
        throw std::runtime_error("use session-open for persistent configuration");
    validate_state_separation(c, state_root_);
    if (!fs::is_regular_file(launcher_) || ::access(launcher_.c_str(), X_OK) < 0)
        throw std::runtime_error("launcher is not executable: " + launcher_.string());
    RunSession session(std::move(c), state_root_, launcher_);
    return session.run();
}
Json SandboxService::validate(const fs::path &file, bool allow) const {
    auto c = RunConfig::load(file);
    c.validate(allow);
    validate_state_separation(c, state_root_);
    return {{"valid", true}, {"effective_config", c.to_json()}, {"execution_verified", false}};
}
Json SandboxService::doctor(const fs::path &root) const {
    utsname u{};
    checked(::uname(&u), "uname");
    errno = 0;
    long result = ::syscall(SYS_clone3, nullptr, 0);
    int error = errno;
    Json j = {{"kernel", u.release},
              {"architecture", u.machine},
              {"uid", ::getuid()},
              {"clone3_syscall_visible", result >= 0 || error == EINVAL || error == EFAULT},
              {"clone3_probe_errno", error},
              {"seccomp_query", ::prctl(PR_GET_SECCOMP)},
              {"launcher", launcher_.string()},
              {"launcher_executable", ::access(launcher_.c_str(), X_OK) == 0},
              {"state_root", state_root_.string()},
              {"execution_verified", false},
              {"note", "Capability probes are not an isolation test; run integration tests in the "
                       "supported Linux environment."}};
    try {
        j["max_user_namespaces"] = read_file("/proc/sys/user/max_user_namespaces", 128);
    } catch (const std::exception &e) {
        j["user_namespace_error"] = e.what();
    }
    if (!root.empty())
        j["cgroup"] = CgroupV2::probe(root);
    for (auto item :
         {std::pair{"apparmor_restrict_unprivileged_userns",
                    "/proc/sys/kernel/apparmor_restrict_unprivileged_userns"},
          std::pair{"unprivileged_userns_clone", "/proc/sys/kernel/unprivileged_userns_clone"},
          std::pair{"apparmor_context", "/proc/self/attr/current"}}) {
        try {
            j["security_policy"][item.first] = read_file(item.second, 1024);
        } catch (const std::exception &e) {
            j["security_policy"][item.first] = nullptr;
        }
    }
    try {
        j["isolation_probe"] = probe_isolation_environment(launcher_);
    } catch (const std::exception &e) {
        j["isolation_probe"] = {
            {"available", false}, {"stage", "probe_process"}, {"error", e.what()}};
    }
    j["deployment"] = {
        {"launcher_layout", launcher_ == fs::path("/opt/linux-sandbox/bin/sandbox-launcher")
                                ? "managed_installation"
                                : "development_or_custom"},
        {"running_as_root", ::geteuid() == 0},
        {"runtime_elevation", false},
        {"guide", "docs/INSTALL.md"}};
    if (!j["isolation_probe"].value("available", false))
        j["isolation_probe"]["remedy"] = probe_remedy(j["isolation_probe"], j["security_policy"]);
    return j;
}
Json SandboxService::inspect(const std::string &id) const {
    return RunStore::inspect(state_root_, id);
}
Json SandboxService::cancel(const std::string &id) const {
    return ControlServer::cancel(state_root_, id);
}
Json SandboxService::cleanup() const {
    return RunStore::cleanup(state_root_);
}
Json SandboxService::prune(std::size_t keep, std::uint64_t older_ms, bool dry_run) {
    return RunStore::prune(state_root_, keep, older_ms, dry_run);
}
} // namespace sandbox
