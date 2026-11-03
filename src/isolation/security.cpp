#include "sandbox/isolation/isolation.hpp"
#include <cerrno>
#include <linux/capability.h>
#include <memory>
#include <sched.h>
#include <seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace sandbox {
namespace {
// The rule set below is a policy, not a constant: when it changes, this number
// changes with it. A result that names the policy but not the rule set cannot be
// compared against a result produced by a different build.
constexpr std::uint32_t baseline_rule_set_version = 1;
} // namespace

SecurityPolicyFacts security_policy_id(const RunConfig &config) {
    // Only one policy exists and the configuration rejects anything else, so the
    // identity is the configured name plus the version of the rules that are
    // actually installed. An unknown name would mean nothing was enforced.
    if (config.seccomp != "baseline")
        return SecurityPolicyFacts{};
    return SecurityPolicyFacts{"baseline", baseline_rule_set_version};
}

void SecurityPolicy::drop_capabilities() {
    // No credential inheritance across exec, including ambient capabilities.
    checked(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), "no_new_privs");
    for (int cap = 0; cap <= CAP_LAST_CAP; ++cap) {
        if (::prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL && errno != EPERM)
            system_error("cap bounding drop");
    }
    if (::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0 && errno != EINVAL)
        system_error("ambient cap clear");
    __user_cap_header_struct h{};
    h.version = _LINUX_CAPABILITY_VERSION_3;
    __user_cap_data_struct data[2]{};
    checked(static_cast<int>(::syscall(SYS_capset, &h, data)), "clear capabilities");
    rlimit core{0, 0};
    checked(::setrlimit(RLIMIT_CORE, &core), "disable core dump");
}
void SecurityPolicy::reset_target_signals() {
    sigset_t empty;
    ::sigemptyset(&empty);
    checked(::sigprocmask(SIG_SETMASK, &empty, nullptr), "restore target signal mask");
    for (int sig = 1; sig < NSIG; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP)
            continue;
        struct sigaction a{};
        a.sa_handler = SIG_DFL;
        ::sigemptyset(&a.sa_mask);
        if (::sigaction(sig, &a, nullptr) < 0 && errno != EINVAL)
            system_error("restore target signals");
    }
}
void SecurityPolicy::install_baseline() {
    // Compatibility-oriented deny policy. This is not a complete malicious-code boundary.
    auto ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (!ctx)
        throw std::runtime_error("seccomp_init failed");
    struct Release {
        scmp_filter_ctx ctx;
        ~Release() {
            seccomp_release(ctx);
        }
    } guard{ctx};
    auto rule = [&](const char *name, std::uint32_t action) {
        int nr = seccomp_syscall_resolve_name(name);
        if (nr == __NR_SCMP_ERROR)
            return;
        int r = seccomp_rule_add(ctx, action, nr, 0);
        if (r < 0)
            throw std::runtime_error("seccomp rule failed: " + std::string(name));
    };
    for (auto name : {"mount",
                      "umount2",
                      "pivot_root",
                      "chroot",
                      "unshare",
                      "setns",
                      "ptrace",
                      "process_vm_readv",
                      "process_vm_writev",
                      "bpf",
                      "perf_event_open",
                      "keyctl",
                      "add_key",
                      "request_key",
                      "open_by_handle_at",
                      "name_to_handle_at",
                      "kexec_load",
                      "kexec_file_load",
                      "reboot",
                      "init_module",
                      "finit_module",
                      "delete_module",
                      "userfaultfd",
                      "io_uring_setup",
                      "fsopen",
                      "fsconfig",
                      "fsmount",
                      "move_mount",
                      "open_tree",
                      "mount_setattr"})
        rule(name, SCMP_ACT_ERRNO(EPERM));
    // glibc can fall back to clone; pointer-based clone3 arguments cannot be inspected by classic
    // seccomp.
    rule("clone3", SCMP_ACT_ERRNO(ENOSYS));
    for (auto bit : {CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWNET, CLONE_NEWIPC,
                     CLONE_NEWUTS, CLONE_NEWCGROUP}) {
        scmp_arg_cmp comparison{0, SCMP_CMP_MASKED_EQ, static_cast<scmp_datum_t>(bit),
                                static_cast<scmp_datum_t>(bit)};
        int r = seccomp_rule_add_array(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), 1, &comparison);
        if (r < 0)
            throw std::runtime_error("seccomp clone mask rule failed");
    }
    int r = seccomp_load(ctx);
    if (r < 0)
        throw std::runtime_error("seccomp_load failed: " + std::to_string(-r));
}
} // namespace sandbox
