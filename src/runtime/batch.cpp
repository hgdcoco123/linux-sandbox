#include "sandbox/runtime/batch.hpp"
#include "sandbox/platform/event_loop.hpp"
#include "sandbox/runtime/session_protocol.hpp"
#include "sandbox/storage/store.hpp"
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace sandbox {
namespace {
bool overlaps(const fs::path &a, const fs::path &b) {
    auto x = a.begin(), y = b.begin();
    while (x != a.end() && y != b.end() && *x == *y) {
        ++x;
        ++y;
    }
    return x == a.end() || y == b.end();
}
void validate_separation(const BatchSpec &spec, const fs::path &state) {
    auto root = fs::weakly_canonical(fs::absolute(state));
    for (const auto &task : spec.tasks) {
        for (const auto &mount : task.config.mounts)
            if (overlaps(root, fs::canonical(mount.source)))
                throw std::invalid_argument("batch metadata overlaps an exposed task directory");
        if (!task.config.runtime_root.empty() &&
            overlaps(root, fs::canonical(task.config.runtime_root)))
            throw std::invalid_argument("batch metadata overlaps a runtime");
    }
    for (std::size_t i = 0; i < spec.tasks.size(); ++i)
        for (std::size_t k = i + 1; k < spec.tasks.size(); ++k)
            for (const auto &a : spec.tasks[i].config.mounts)
                for (const auto &b : spec.tasks[k].config.mounts)
                    if ((!a.readonly || !b.readonly) &&
                        overlaps(fs::canonical(a.source), fs::canonical(b.source)))
                        throw std::invalid_argument(
                            "independent batch tasks cannot share writable mount trees");
}
struct Child {
    pid_t pid = -1;
    UniqueFd pidfd;
    bool reaped = false;
    ~Child() {
        // Exceptional unwinding retains ownership until the supervisor exits.
        // A blocked filesystem syscall has no safe hard thread/process cleanup deadline.
        if (pid > 0 && !reaped) {
            ::kill(pid, SIGTERM);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }
    void stop() {
        if (!reaped)
            send_pidfd_signal(pidfd.get(), SIGTERM);
    }
};
std::unique_ptr<Child> spawn_task(const std::vector<std::string> &arguments,
                                  const fs::path &directory) {
    auto child = std::make_unique<Child>();
    auto out = open_path(directory / "supervisor.stdout", O_WRONLY | O_CREAT | O_EXCL);
    auto err = open_path(directory / "supervisor.stderr", O_WRONLY | O_CREAT | O_EXCL);
    auto in = open_path("/dev/null", O_RDONLY);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    auto check = [](int code) {
        if (code) {
            errno = code;
            system_error("batch posix_spawn");
        }
    };
    check(::posix_spawn_file_actions_init(&actions));
    struct Actions {
        posix_spawn_file_actions_t *p;
        ~Actions() {
            ::posix_spawn_file_actions_destroy(p);
        }
    } actions_guard{&actions};
    check(::posix_spawnattr_init(&attr));
    struct Attr {
        posix_spawnattr_t *p;
        ~Attr() {
            ::posix_spawnattr_destroy(p);
        }
    } attr_guard{&attr};
    check(::posix_spawn_file_actions_adddup2(&actions, in.get(), 0));
    check(::posix_spawn_file_actions_adddup2(&actions, out.get(), 1));
    check(::posix_spawn_file_actions_adddup2(&actions, err.get(), 2));
    check(::posix_spawn_file_actions_addclosefrom_np(&actions, 3));
    sigset_t mask, defaults;
    ::sigemptyset(&mask);
    ::sigemptyset(&defaults);
    for (int sig : {SIGINT, SIGTERM, SIGCHLD, SIGPIPE})
        ::sigaddset(&defaults, sig);
    check(::posix_spawnattr_setsigmask(&attr, &mask));
    check(::posix_spawnattr_setsigdefault(&attr, &defaults));
    check(::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    std::vector<char *> argv;
    for (const auto &arg : arguments)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    check(::posix_spawn(&child->pid, argv[0], &actions, &attr, argv.data(), environ));
    child->pidfd.reset(
        checked(static_cast<int>(::syscall(SYS_pidfd_open, child->pid, 0)), "batch pidfd_open"));
    return child;
}
fs::path find_run(const fs::path &root) {
    fs::path found;
    for (const auto &entry : fs::directory_iterator(root)) {
        auto name = entry.path().filename().string();
        if (name.rfind("run-", 0) != 0)
            continue;
        if (!found.empty())
            throw std::runtime_error("ambiguous task run directory");
        found = RunStore::verified_run(root, name);
    }
    return found;
}
} // namespace
Json run_batch(BatchSpec spec, const fs::path &state, const fs::path &binary,
               const fs::path &launcher) {
    validate_separation(spec, state);
    ensure_standard_fds();
    SignalSource signals;
    EventLoop loop;
    TickSource ticks;
    Scheduler scheduler(spec);
    RunConfig owner_config;
    owner_config.mode = "process";
    auto store = RunStore::create(state, owner_config);
    store->owner_update({{"batch", true}});
    auto base = store->directory();
    // Child runs need a short private socket path regardless of the caller's state
    // path. Retain this root with the reports; record it explicitly for retention/recovery.
    char pattern[] = "/tmp/sbx-tasks-XXXXXX";
    auto temp = ::mkdtemp(pattern);
    if (!temp)
        system_error("create task execution root");
    fs::path execution_root = temp;
    store->owner_update({{"task_state_root", execution_root.string()}});
    checked(::mkdir((base / "tasks").c_str(), 0700), "create batch tasks");
    struct TaskData {
        fs::path root, run;
        Json report = nullptr;
        bool cancel = false;
        std::string error;
    };
    std::vector<TaskData> data(spec.tasks.size());
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < spec.tasks.size(); ++i) {
        indices.emplace(spec.tasks[i].id, i);
        data[i].root = execution_root / std::to_string(i);
        checked(::mkdir(data[i].root.c_str(), 0700), "create task directory");
        checked(::mkdir((base / "tasks" / spec.tasks[i].id).c_str(), 0700),
                "create task config directory");
        store->write_json("tasks/" + spec.tasks[i].id + "/config.json",
                          spec.tasks[i].config.to_json());
    }
    std::map<std::size_t, std::unique_ptr<Child>> active;
    ControlServer control(base / "control.sock");
    bool stopping = false, finalized = false;
    std::size_t peak_active = 0;
    std::uint64_t peak_memory = 0, peak_pids = 0;
    auto snapshot = [&] {
        Json tasks = Json::array();
        for (std::size_t i = 0; i < spec.tasks.size(); ++i) {
            Json task = {{"id", spec.tasks[i].id},
                         {"state", task_state_name(scheduler.state(i))},
                         {"cancel_requested", data[i].cancel},
                         {"error", data[i].error},
                         {"task_state_root", data[i].root.string()}};
            task["run_id"] =
                data[i].run.empty() ? Json(nullptr) : Json(data[i].run.filename().string());
            task["summary"] = data[i].report.is_object()
                                  ? Json(data[i].report.value("summary", "unknown"))
                                  : Json(nullptr);
            tasks.push_back(std::move(task));
        }
        auto used = scheduler.resources().used();
        return Json{{"schema_version", 1},
                    {"run_id", store->id()},
                    {"batch_state", finalized ? "finished" : (stopping ? "cancelling" : "running")},
                    {"tasks", tasks},
                    {"task_state_root", execution_root.string()},
                    {"active", scheduler.resources().active()},
                    {"reserved_memory_bytes", used.memory},
                    {"reserved_pids", used.pids},
                    {"peak_active", peak_active},
                    {"peak_reserved_memory_bytes", peak_memory},
                    {"peak_reserved_pids", peak_pids}};
    };
    auto persist = [&] { store->write_json("batch.json", snapshot()); };
    auto cancel = [&](std::size_t i) {
        data[i].cancel = true;
        if (!scheduler.cancel_queued(i)) {
            auto found = active.find(i);
            if (found != active.end())
                found->second->stop();
        }
    };
    store->state(RunState::running);
    persist();
    while (!scheduler.done()) {
        bool changed = false;
        if (signals.drain_cancel()) {
            stopping = true;
            for (std::size_t i = 0; i < data.size(); ++i)
                cancel(i);
            changed = true;
        }
        control.serve({}, [&](const Json &j) -> Json {
            auto version = j.value("protocol_version", 1);
            if (version != 1 && version != 2)
                throw SessionError("UNSUPPORTED_VERSION", "unsupported batch control version");
            auto op = j.at("op").get<std::string>();
            if (op == "status") {
                // Large batches use per-task lookup; do not silently truncate status.
                if (j.contains("task_id")) {
                    auto it = indices.find(j.at("task_id").get<std::string>());
                    if (it == indices.end())
                        throw SessionError("TASK_NOT_FOUND", "unknown task id");
                    return snapshot().at("tasks").at(it->second);
                }
                return snapshot();
            }
            if (op != "cancel" && op != "cancel-task")
                throw SessionError("UNKNOWN_OPERATION",
                                   "batch supports status, cancel and cancel-task");
            if (op == "cancel-task") {
                auto it = indices.find(j.at("task_id").get<std::string>());
                if (it == indices.end())
                    throw SessionError("TASK_NOT_FOUND", "unknown task id");
                cancel(it->second);
            } else {
                stopping = true;
                for (std::size_t i = 0; i < data.size(); ++i)
                    cancel(i);
            }
            persist();
            return {{"status", "accepted"}};
        });
        for (auto it = active.begin(); it != active.end();) {
            auto i = it->first;
            if (data[i].run.empty())
                data[i].run = find_run(data[i].root);
            if (!data[i].run.empty() && scheduler.state(i) == TaskState::starting) {
                scheduler.running(i);
                changed = true;
            }
            if (!data[i].run.empty() && fs::exists(data[i].run / "result.json") &&
                scheduler.state(i) != TaskState::finishing) {
                scheduler.finishing(i);
                changed = true;
            }
            int status = 0;
            auto reaped = ::waitpid(it->second->pid, &status, WNOHANG);
            if (reaped < 0 && errno != EINTR)
                system_error("batch waitpid");
            if (reaped <= 0) {
                ++it;
                continue;
            }
            it->second->reaped = true;
            if (data[i].run.empty())
                data[i].run = find_run(data[i].root);
            auto terminal = TaskState::failed;
            try {
                if (data[i].run.empty())
                    throw std::runtime_error("task exited before publishing run identity");
                data[i].report = Json::parse(read_file(data[i].run / "result.json"));
                auto summary = data[i].report.at("summary").get<std::string>();
                if (summary == "success")
                    terminal = TaskState::completed;
                else if (summary == "cancelled")
                    terminal = TaskState::cancelled;
                // A task whose target succeeded but whose environment was not
                // reclaimed is not a completed task, even though the child's
                // summary says success.
                const auto verdict = data[i].report.value("verdict", std::string("ok"));
                if (terminal == TaskState::completed && verdict != "ok")
                    throw std::runtime_error("task environment not reclaimed: " + verdict);
                if (!WIFEXITED(status))
                    throw std::runtime_error("task supervisor terminated by signal");
                // Success report with failing supervisor exit is not success.
                if (terminal == TaskState::completed && WEXITSTATUS(status) != 0)
                    throw std::runtime_error("task report/exit status mismatch");
            } catch (const std::exception &e) {
                terminal = TaskState::failed;
                data[i].error = e.what();
            }
            scheduler.complete(i, terminal); // reservation held until supervisor has exited
            it = active.erase(it);
            changed = true;
        }
        if (!stopping)
            while (auto index = scheduler.admit()) {
                auto i = *index;
                try {
                    std::vector<std::string> argv{
                        binary.string(),
                        "run",
                        "--config",
                        (base / "tasks" / spec.tasks[i].id / "config.json").string(),
                        "--state-root",
                        data[i].root.string(),
                        "--launcher",
                        launcher.string()};
                    if (spec.tasks[i].config.mode == "process")
                        argv.push_back("--allow-unsafe");
                    active.emplace(i, spawn_task(argv, data[i].root));
                } catch (const std::exception &e) {
                    data[i].error = e.what();
                    scheduler.complete(i, TaskState::failed);
                }
                changed = true;
                peak_active = std::max(peak_active, scheduler.resources().active());
                peak_memory = std::max(peak_memory, scheduler.resources().used().memory);
                peak_pids = std::max(peak_pids, scheduler.resources().used().pids);
            }
        if (changed)
            persist();
        if (scheduler.done())
            break;
        std::vector<EventWatch> watches{{signals.fd(), 1}, {ticks.fd(), 2}, {control.fd(), 3}};
        for (auto &[i, child] : active)
            watches.push_back({child->pidfd.get(), static_cast<std::uint32_t>(i + 10)});
        loop.wait(watches);
        ticks.drain();
    }
    finalized = true;
    auto result = snapshot();
    bool success = true;
    for (std::size_t i = 0; i < data.size(); ++i)
        if (scheduler.state(i) != TaskState::completed)
            success = false;
    result["summary"] = success ? "success" : "incomplete";
    store->write_json("batch.json", result);
    store->write_json("result.json", result);
    store->state(RunState::finished);
    return result;
}
} // namespace sandbox
