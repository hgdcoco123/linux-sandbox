#pragma once
#include "sandbox/task.hpp"
namespace sandbox {
Json run_batch(BatchSpec, const fs::path &state, const fs::path &binary, const fs::path &launcher);
} // namespace sandbox
