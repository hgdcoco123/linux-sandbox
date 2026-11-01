#pragma once
#include "sandbox/core/workspace.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace sandbox {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct MountSpec {
    fs::path source;
    std::string target;
    bool readonly = true;
};
struct Limits {
    std::uint64_t memory_bytes = 256ULL * 1024 * 1024;
    std::uint64_t swap_bytes = 0;
    std::uint64_t pids = 64;
    std::uint64_t cpu_quota_us = 100000;
    std::uint64_t cpu_period_us = 100000;
    std::uint64_t wall_ms = 30000;
    std::uint64_t startup_ms = 5000;
    std::uint64_t grace_ms = 500;
    std::uint64_t capture_bytes = 8ULL * 1024 * 1024;
    std::uint64_t tmp_bytes = 32ULL * 1024 * 1024;
};
struct RunConfig {
    bool persistent = false;
    std::uint64_t session_ms = 3600000, idle_ms = 300000;
    struct SamplingConfig {
        std::uint64_t interval_ms = 250;
        std::size_t history_limit = 256;
    } metrics;
    std::vector<std::string> argv;
    fs::path runtime_root, stdin_file;
    std::string cwd = "/work";
    std::map<std::string, std::string> env{{"PATH", "/usr/bin:/bin"}, {"LANG", "C.UTF-8"}};
    std::vector<MountSpec> mounts;
    Limits limits;
    // What may enter the sandbox workspace and what survives teardown.
    WorkspacePolicy workspace;
    // Host directory bound into the sandbox at /transfer when the workspace is a
    // quota-enforced tmpfs. A tmpfs mounted inside the sandbox namespace is
    // invisible to the supervisor, so the sandbox moves its own data through this
    // directory instead. Empty means the workspace is the plain host directory.
    fs::path workspace_transfer;
    std::string mode = "isolated";
    std::string network = "none";
    std::string seccomp = "baseline";
    fs::path cgroup_root;
    Json to_json() const;
    static RunConfig parse(const Json &, const fs::path &base);
    static RunConfig load(const fs::path &file);
    void validate(bool allow_process) const;
};
void require_keys(const Json &, std::initializer_list<const char *> allowed,
                  const std::string &where);
} // namespace sandbox
