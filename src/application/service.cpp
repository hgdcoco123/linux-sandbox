#include "sandbox/service.hpp"
#include "sandbox/environment.hpp"
#include <cerrno>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
namespace sandbox {
namespace {
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
RunReport SandboxService::run(const fs::path &file, bool allow) {
    auto c = RunConfig::load(file);
    c.validate(allow);
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
        {"guide", "docs/DEPLOYMENT.md"}};
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
} // namespace sandbox
