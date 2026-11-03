#pragma once
#include "sandbox/runtime/session.hpp"
namespace sandbox {
class SandboxService {
  public:
    SandboxService(fs::path state_root, fs::path launcher)
        : state_root_(std::move(state_root)), launcher_(std::move(launcher)) {}
    RunReport run(const fs::path &config, bool allow_unsafe);
    Json validate(const fs::path &config, bool allow_unsafe) const;
    Json doctor(const fs::path &cgroup_root = {}) const;
    Json inspect(const std::string &id) const;
    Json cancel(const std::string &id) const;
    Json cleanup() const;

  private:
    fs::path state_root_, launcher_;
};
} // namespace sandbox
