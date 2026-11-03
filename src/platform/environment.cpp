#include "sandbox/platform/environment.hpp"
#include "sandbox/platform/linux.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/sched.h>
#include <poll.h>
#include <sched.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
namespace sandbox {
namespace {
void print_probe(Json result, const std::string &stage) {
    result["stage"] = stage;
    try {
        result["apparmor_context"] = read_file("/proc/self/attr/current", 1024);
    } catch (...) {
    }
    std::cout << result.dump() << '\n' << std::flush;
}
} // namespace
int launcher_environment_probe() {
    Json result = {{"available", false},
                   {"mechanism", "clone3_parent_mapping"},
                   {"scope", "new_user_mount_pid_ipc_uts_network_namespaces"},
                   {"host_policy_modified", false},
                   {"execution_verified", false}};
    const auto uid = ::getuid(), gid = ::getgid();
    std::string stage = "parent_watch";
    pid_t child = -1;
    bool reaped = false;
    try {
        const auto parent = ::getppid();
        checked(::prctl(PR_SET_PDEATHSIG, SIGKILL), "probe parent death");
        if (parent <= 1 || ::getppid() != parent)
            throw std::runtime_error("probe parent exited");
        auto gate = Pipe::create();
        int pidfd = -1;
        clone_args args{};
        args.flags = CLONE_PIDFD | CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC |
                     CLONE_NEWUTS | CLONE_NEWNET;
        args.pidfd = reinterpret_cast<std::uint64_t>(&pidfd);
        args.exit_signal = SIGCHLD;
        stage = "create_user_mount_namespace";
        child = static_cast<pid_t>(::syscall(SYS_clone3, &args, sizeof(args)));
        checked(child, "clone3 environment probe");
        if (child == 0) {
            gate.write.reset();
            if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
                ::_exit(125);
            char ready;
            ssize_t n;
            do {
                n = ::read(gate.read.get(), &ready, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1)
                ::_exit(125);
            if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
                auto error = errno;
                result["errno"] = error;
                result["error"] = std::strerror(error);
            } else
                result["available"] = true;
            print_probe(result,
                        result.at("available").get<bool>() ? "completed" : "make_mounts_private");
            ::_exit(0);
        }
        UniqueFd child_ref(pidfd);
        gate.read.reset();
        stage = "uid_gid_mapping";
        auto proc = fs::path("/proc") / std::to_string(child);
        write_control_file(proc / "setgroups", "deny");
        write_control_file(proc / "uid_map", "0 " + std::to_string(uid) + " 1");
        write_control_file(proc / "gid_map", "0 " + std::to_string(gid) + " 1");
        write_all(gate.write.get(), "x");
        gate.write.reset();
        stage = "probe_child";
        int status;
        pid_t waited;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        checked(waited, "wait environment probe child");
        reaped = true;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("environment probe child exited abnormally");
        return 0; // The child produced the sole JSON response, with its actual confinement context.
    } catch (const std::exception &e) {
        if (child > 0 && !reaped) {
            ::kill(child, SIGKILL);
            while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        if (auto system = dynamic_cast<const std::system_error *>(&e))
            result["errno"] = system->code().value();
        result["error"] = e.what();
        result["note"] = "The required namespace or mapping capability was denied. No policy was "
                         "changed, no elevation or fallback was attempted.";
        print_probe(result, stage);
        return 0;
    }
}

Json probe_isolation_environment(const fs::path &launcher) {
    auto pipe = Pipe::create();
    nonblocking(pipe.read.get());
    // Use the actual launcher executable, so its path-specific confinement applies.
    auto output =
        UniqueFd(checked(::fcntl(pipe.write.get(), F_DUPFD_CLOEXEC, 100), "probe output fd"));
    auto null = open_path("/dev/null", O_RDWR);
    auto null_copy = UniqueFd(checked(::fcntl(null.get(), F_DUPFD_CLOEXEC, 100), "probe null fd"));
    posix_spawn_file_actions_t actions;
    int rc = ::posix_spawn_file_actions_init(&actions);
    if (rc) {
        errno = rc;
        system_error("probe actions init");
    }
    struct Guard {
        posix_spawn_file_actions_t *p;
        ~Guard() {
            ::posix_spawn_file_actions_destroy(p);
        }
    } guard{&actions};
    auto check = [](int code) {
        if (code) {
            errno = code;
            system_error("probe spawn setup");
        }
    };
    check(::posix_spawn_file_actions_adddup2(&actions, output.get(), 1));
    check(::posix_spawn_file_actions_adddup2(&actions, null_copy.get(), 0));
    check(::posix_spawn_file_actions_adddup2(&actions, null_copy.get(), 2));
    check(::posix_spawn_file_actions_addclosefrom_np(&actions, 3));
    auto executable = launcher.string();
    char flag[] = "--probe-environment";
    char *argv[]{executable.data(), flag, nullptr};
    char path[] = "PATH=/usr/bin:/bin";
    char *env[]{path, nullptr};
    pid_t pid = -1;
    check(::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv, env));
    pipe.write.reset();
    output.reset();
    struct Child {
        pid_t pid;
        bool reaped = false;
        ~Child() {
            if (!reaped) {
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
        }
    } child{pid};
    std::string data;
    const auto deadline = monotonic_ms() + 3000;
    bool eof = false;
    int status = 0;
    while (monotonic_ms() < deadline) {
        char bytes[2048];
        auto n = ::read(pipe.read.get(), bytes, sizeof(bytes));
        if (n > 0) {
            data.append(bytes, n);
            if (data.size() > 16384)
                throw std::runtime_error("probe response too large");
        } else if (n == 0)
            eof = true;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            system_error("read probe");
        if (!child.reaped) {
            auto waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                child.reaped = true;
            else if (waited < 0 && errno != EINTR)
                system_error("wait probe");
        }
        if (child.reaped && eof) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                throw std::runtime_error("environment probe exited abnormally");
            return Json::parse(data);
        }
        pollfd fd{pipe.read.get(), POLLIN, 0};
        ::poll(&fd, 1, 20);
    }
    throw std::runtime_error("environment probe exceeded 3 seconds");
}
} // namespace sandbox
