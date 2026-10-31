#include "sandbox/cgroup.hpp"
#include <sstream>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <thread>
#include <unistd.h>
namespace sandbox {
namespace {
Json kv(const fs::path &p) {
    Json j = Json::object();
    std::istringstream in(read_file(p));
    std::string k;
    std::uint64_t n;
    while (in >> k >> n)
        j[k] = n;
    return j;
}
void verify_root(const fs::path &p) {
    auto fd = open_dir(p);
    struct statfs s{};
    checked(::fstatfs(fd.get(), &s), "statfs cgroup");
    if (s.f_type != 0x63677270)
        throw std::runtime_error("cgroup_root is not cgroup v2");
    auto canonical = fs::canonical(p);
    if (canonical == "/sys/fs/cgroup")
        throw std::runtime_error("refusing global cgroup root; provide delegated empty parent");
    if (read_file(p / "cgroup.procs") != "")
        throw std::runtime_error("delegated parent must contain no processes");
    auto enabled = read_file(p / "cgroup.subtree_control");
    for (auto c : {"cpu", "memory", "pids"})
        if (enabled.find(c) == std::string::npos)
            throw std::runtime_error("controller not enabled in delegated parent: " +
                                     std::string(c));
}
void set_value(const fs::path &p, const std::string &value) {
    write_control_file(p, value);
}
} // namespace
CgroupV2::CgroupV2(const fs::path &delegated, const std::string &id, const Limits &l) {
    verify_root(delegated);
    path_ = fs::canonical(delegated) / id;
    checked(::mkdir(path_.c_str(), 0700), "mkdir cgroup");
    try {
        set_value(path_ / "memory.max", std::to_string(l.memory_bytes));
        set_value(path_ / "memory.swap.max", std::to_string(l.swap_bytes));
        set_value(path_ / "memory.oom.group", "1");
        set_value(path_ / "cpu.max",
                  std::to_string(l.cpu_quota_us) + " " + std::to_string(l.cpu_period_us));
        set_value(path_ / "pids.max", std::to_string(l.pids));
        if (!fs::exists(path_ / "cgroup.kill"))
            throw std::runtime_error("cgroup.kill unavailable");
    } catch (...) {
        ::rmdir(path_.c_str());
        throw;
    }
}
CgroupV2::~CgroupV2() {
    if (!removed_) {
        try {
            kill_all();
            if (empty())
                ::rmdir(path_.c_str());
        } catch (...) {
        }
    }
}
void CgroupV2::attach(pid_t p) {
    set_value(path_ / "cgroup.procs", std::to_string(p));
}
bool CgroupV2::empty() const {
    return kv(path_ / "cgroup.events").at("populated") == 0;
}
void CgroupV2::kill_all() {
    set_value(path_ / "cgroup.kill", "1");
}
void CgroupV2::remove() {
    if (!empty())
        throw std::runtime_error("refusing removal of populated cgroup");
    checked(::rmdir(path_.c_str()), "remove cgroup");
    removed_ = true;
}
Json CgroupV2::probe(const fs::path &root) {
    Json j = {{"path", root.string()}};
    try {
        verify_root(root);
        j["ready"] = true;
    } catch (const std::exception &e) {
        j["ready"] = false;
        j["error"] = e.what();
    }
    return j;
}
void CgroupV2::recover(const fs::path &root, const std::string &id) {
    verify_root(root);
    auto p = fs::canonical(root) / id;
    if (!fs::exists(p))
        return;
    auto fd = open_dir(p);
    set_value(p / "cgroup.kill", "1");
    auto deadline = monotonic_ms() + 3000;
    while (kv(p / "cgroup.events").at("populated") != 0) {
        if (monotonic_ms() > deadline)
            throw std::runtime_error("recovery kill deadline exceeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    checked(::rmdir(p.c_str()), "remove stale cgroup");
}
} // namespace sandbox
