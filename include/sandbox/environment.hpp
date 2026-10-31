#pragma once
#include "sandbox/config.hpp"
namespace sandbox {
Json probe_isolation_environment(const fs::path &launcher);
int launcher_environment_probe();
} // namespace sandbox
