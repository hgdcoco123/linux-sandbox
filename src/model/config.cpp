#include "sandbox/config.hpp"
#include <stdexcept>
#include <fstream>
#include <set>
#include <stdexcept>
namespace sandbox {
void require_keys(const Json &j, std::initializer_list<const char *> allowed,
                  const std::string &where) {
    if (!j.is_object())
        throw std::runtime_error(where + " must be an object");
    std::set<std::string> keys;
    for (auto k : allowed)
        keys.emplace(k);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!keys.contains(it.key()))
            throw std::runtime_error(where + ": unknown field " + it.key());
}
namespace {
std::uint64_t numeric(const Json &j, const char *key, std::uint64_t def, std::uint64_t max) {
    if (!j.contains(key))
        return def;
    const auto &value = j.at(key);
    if (!(value.is_number_unsigned() ||
          (value.is_number_integer() && value.get<std::int64_t>() >= 0)))
        throw std::runtime_error(std::string(key) + " must be a nonnegative integer");
    auto n = value.get<std::uint64_t>();
    if (n > max)
        throw std::runtime_error(std::string(key) + " too large");
    return n;
}
fs::path path_at(const fs::path &base, const std::string &value) {
    if (value.empty())
        return {};
    auto p = fs::path(value);
    return fs::absolute(p.is_absolute() ? p : base / p).lexically_normal();
}
bool contains_nul(const std::string &s) {
    return s.find('\0') != std::string::npos;
}
bool beneath(const fs::path &p, const fs::path &root) {
    auto a = p.begin(), b = root.begin();
    for (; b != root.end(); ++b, ++a)
        if (a == p.end() || *a != *b)
            return false;
    return true;
}
} // namespace
RunConfig RunConfig::load(const fs::path &file) {
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot read config: " + file.string());
    Json j;
    in >> j;
    return parse(j, fs::absolute(file).parent_path());
}
RunConfig RunConfig::parse(const Json &j, const fs::path &base) {
    require_keys(j,
                 {"schema_version", "argv", "runtime_root", "cwd", "stdin_file", "env", "mounts",
                  "limits", "mode", "network", "seccomp", "cgroup_root", "metrics", "session", "workspace",
                  "workspace_transfer"},
                 "config");
    if (!j.contains("schema_version") || j.at("schema_version") != 1)
        throw std::runtime_error("schema_version must be 1");
    RunConfig c;
    auto session = j.value("session", Json::object());
    require_keys(session, {"enabled", "total_ms", "idle_ms"}, "session");
    c.persistent = session.value("enabled", false);
    c.session_ms = numeric(session, "total_ms", c.session_ms, 86400000);
    c.idle_ms = numeric(session, "idle_ms", c.idle_ms, 86400000);
    c.argv = j.at("argv").get<std::vector<std::string>>();
    c.runtime_root = path_at(base, j.value("runtime_root", ""));
    c.stdin_file = path_at(base, j.value("stdin_file", ""));
    c.cgroup_root = path_at(base, j.value("cgroup_root", ""));
    c.cwd = j.value("cwd", "/work");
    c.mode = j.value("mode", "isolated");
    c.network = j.value("network", "none");
    c.seccomp = j.value("seccomp", "baseline");
    // Internal: only a supervisor that owns a quota-enforced workspace sets this.
    // Accepted from a file because the effective configuration travels to init as
    // JSON, but validated hard below.
    if (j.contains("workspace_transfer")) {
        if (!j.at("workspace_transfer").is_string())
            throw std::runtime_error("workspace_transfer must be a string");
        c.workspace_transfer = j.at("workspace_transfer").get<std::string>();
    }
    if (j.contains("env"))
        c.env = j.at("env").get<std::map<std::string, std::string>>();
    for (const auto &m : j.value("mounts", Json::array())) {
        require_keys(m, {"source", "target", "readonly"}, "mount");
        c.mounts.push_back({path_at(base, m.at("source").get<std::string>()),
                            m.at("target").get<std::string>(), m.value("readonly", true)});
    }
    auto l = j.value("limits", Json::object());
    require_keys(l,
                 {"memory_bytes", "swap_bytes", "pids", "cpu_quota_us", "cpu_period_us", "wall_ms",
                  "startup_ms", "grace_ms", "capture_bytes", "tmp_bytes"},
                 "limits");
    auto &v = c.limits;
    v.memory_bytes = numeric(l, "memory_bytes", v.memory_bytes, 1ULL << 50);
    v.swap_bytes = numeric(l, "swap_bytes", v.swap_bytes, 1ULL << 50);
    v.pids = numeric(l, "pids", v.pids, 65536);
    v.cpu_quota_us = numeric(l, "cpu_quota_us", v.cpu_quota_us, 100000000);
    v.cpu_period_us = numeric(l, "cpu_period_us", v.cpu_period_us, 1000000);
    v.wall_ms = numeric(l, "wall_ms", v.wall_ms, 86400000);
    v.startup_ms = numeric(l, "startup_ms", v.startup_ms, 60000);
    v.grace_ms = numeric(l, "grace_ms", v.grace_ms, 10000);
    v.capture_bytes = numeric(l, "capture_bytes", v.capture_bytes, 1ULL << 40);
    v.tmp_bytes = numeric(l, "tmp_bytes", v.tmp_bytes, 1ULL << 40);
    const auto sampling = j.value("metrics", Json::object());
    require_keys(sampling, {"interval_ms", "history_limit"}, "metrics");
    c.metrics.interval_ms = numeric(sampling, "interval_ms", c.metrics.interval_ms, 10000);
    c.metrics.history_limit =
        static_cast<std::size_t>(numeric(sampling, "history_limit", c.metrics.history_limit, 1024));
    const auto workspace = j.value("workspace", Json::object());
    require_keys(workspace, {"max_bytes", "max_entries", "max_depth", "input", "keep_on_close"},
                 "workspace");
    auto &w = c.workspace;
    w.max_bytes = numeric(workspace, "max_bytes", w.max_bytes, 1ULL << 40);
    w.max_entries =
        static_cast<std::uint32_t>(numeric(workspace, "max_entries", w.max_entries, 1u << 20));
    w.max_depth = static_cast<std::uint8_t>(numeric(workspace, "max_depth", w.max_depth, 255));
    if (workspace.contains("input")) {
        const auto input = workspace.at("input").get<std::string>();
        if (input == "bind_readonly")
            w.input = InputMode::bind_readonly;
        else if (input == "snapshot_copy")
            w.input = InputMode::snapshot_copy;
        else
            throw std::runtime_error("workspace.input must be bind_readonly or snapshot_copy");
    }
    w.keep_on_close = workspace.value("keep_on_close", w.keep_on_close);
    return c;
}
void RunConfig::validate(bool allow_process) const {
    try {
        workspace.validate();
    } catch (const std::invalid_argument &e) {
        throw std::runtime_error(std::string("workspace: ") + e.what());
    }
    if (!session_ms || !idle_ms || session_ms > 86400000 || idle_ms > session_ms)
        throw std::runtime_error("session requires 0 < idle_ms <= total_ms <= 86400000");
    if (metrics.interval_ms < 50 || metrics.interval_ms > 10000 || metrics.history_limit == 0 ||
        metrics.history_limit > 1024)
        throw std::runtime_error("metrics: interval_ms must be 50..10000, history_limit 1..1024");
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/')
        throw std::runtime_error("argv[0] must be absolute");
    if (argv.size() > 1024)
        throw std::runtime_error("too many arguments");
    for (const auto &a : argv)
        if (contains_nul(a) || a.size() > 32768)
            throw std::runtime_error("invalid argument");
    if (mode != "isolated" && mode != "process")
        throw std::runtime_error("mode must be isolated/process");
    if (mode == "process" && !allow_process)
        throw std::runtime_error("process mode requires --allow-unsafe; no isolation is provided");
    if (network != "none" && network != "host")
        throw std::runtime_error("network must be none/host");
    if (seccomp != "baseline")
        throw std::runtime_error("only baseline seccomp policy is supported");
    if (cwd.empty() || cwd[0] != '/' || contains_nul(cwd))
        throw std::runtime_error("cwd must be absolute");
    for (auto p : {runtime_root, stdin_file, cgroup_root})
        if (contains_nul(p.string()))
            throw std::runtime_error("NUL in path");
    for (const auto &[k, v] : env)
        if (k.empty() || k.find('=') != std::string::npos || contains_nul(k) || contains_nul(v))
            throw std::runtime_error("invalid environment variable");
    if (!limits.wall_ms || !limits.startup_ms || limits.memory_bytes < 16 * 1024 * 1024 ||
        limits.pids < 4 || limits.cpu_period_us < 1000 || limits.cpu_quota_us < 1000 ||
        !limits.tmp_bytes)
        throw std::runtime_error(
            "invalid limits (memory>=16MiB, pids>=4, positive deadlines/quota/tmp)");
    if (!workspace_transfer.empty()) {
        if (mode != "isolated")
            throw std::runtime_error("workspace_transfer requires isolated mode");
        if (workspace_transfer.is_relative() || contains_nul(workspace_transfer.string()))
            throw std::runtime_error("workspace_transfer must be an absolute path");
    }
    if (mode == "isolated" && (runtime_root.empty() || cgroup_root.empty()))
        throw std::runtime_error("isolated mode needs runtime_root and delegated cgroup_root");
    if (mode == "process" &&
        (!runtime_root.empty() || !mounts.empty() || !cgroup_root.empty() || network != "host"))
        throw std::runtime_error("process mode needs network=host and no runtime/mounts/cgroup; it "
                                 "is a diagnostic backend");
    std::set<std::string> targets;
    for (const auto &m : mounts) {
        if (m.source.empty() || contains_nul(m.source.string()) ||
            (m.target != "/input" && m.target != "/output"))
            throw std::runtime_error("only /input and /output directory mappings are supported");
        if (!targets.insert(m.target).second)
            throw std::runtime_error("duplicate mount target");
        if (m.target == "/input" && !m.readonly)
            throw std::runtime_error("/input must be readonly");
        if (!fs::is_directory(m.source))
            throw std::runtime_error("mount source is not a directory");
        if (!runtime_root.empty()) {
            auto source = fs::canonical(m.source), runtime = fs::canonical(runtime_root);
            if (beneath(source, runtime) || beneath(runtime, source))
                throw std::runtime_error("runtime/mount overlap");
        }
    }
    for (std::size_t i = 0; i < mounts.size(); ++i)
        for (std::size_t k = i + 1; k < mounts.size(); ++k) {
            auto a = fs::canonical(mounts[i].source), b = fs::canonical(mounts[k].source);
            if (beneath(a, b) || beneath(b, a))
                throw std::runtime_error("mount sources overlap");
        }
    if (!stdin_file.empty() && !fs::is_regular_file(stdin_file))
        throw std::runtime_error("stdin_file must be regular");
    if (mode == "isolated") {
        if (!fs::is_directory(runtime_root) || !fs::is_directory(cgroup_root))
            throw std::runtime_error("runtime/cgroup directory missing");
        for (auto sub : {"input", "output", "work", "tmp", "proc", "dev", ".oldroot"}) {
            auto p = runtime_root / sub;
            if (!fs::is_directory(p) || fs::is_symlink(p))
                throw std::runtime_error("runtime needs plain placeholder: " + std::string(sub));
        }
    }
}
Json RunConfig::to_json() const {
    Json ms = Json::array();
    for (const auto &m : mounts)
        ms.push_back(
            {{"source", m.source.string()}, {"target", m.target}, {"readonly", m.readonly}});
    const auto &l = limits;
    return {{"schema_version", 1},
            {"session", {{"enabled", persistent}, {"total_ms", session_ms}, {"idle_ms", idle_ms}}},
            {"argv", argv},
            {"runtime_root", runtime_root.string()},
            {"cwd", cwd},
            {"stdin_file", stdin_file.string()},
            {"env", env},
            {"mounts", ms},
            {"mode", mode},
            {"network", network},
            {"seccomp", seccomp},
            {"cgroup_root", cgroup_root.string()},
            {"metrics",
             {{"interval_ms", metrics.interval_ms}, {"history_limit", metrics.history_limit}}},
            {"workspace",
             {{"max_bytes", workspace.max_bytes},
              {"max_entries", workspace.max_entries},
              {"max_depth", workspace.max_depth},
              {"input", input_mode_name(workspace.input)},
              {"keep_on_close", workspace.keep_on_close}}},
            {"workspace_transfer", workspace_transfer.string()},
            {"limits",
             {{"memory_bytes", l.memory_bytes},
              {"swap_bytes", l.swap_bytes},
              {"pids", l.pids},
              {"cpu_quota_us", l.cpu_quota_us},
              {"cpu_period_us", l.cpu_period_us},
              {"wall_ms", l.wall_ms},
              {"startup_ms", l.startup_ms},
              {"grace_ms", l.grace_ms},
              {"capture_bytes", l.capture_bytes},
              {"tmp_bytes", l.tmp_bytes}}}};
}
} // namespace sandbox
