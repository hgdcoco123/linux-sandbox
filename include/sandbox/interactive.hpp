#pragma once
#include "sandbox/config.hpp"
namespace sandbox {
Json serve_session(RunConfig, const fs::path &state, const fs::path &launcher);
Json file_request(const Json &);
} // namespace sandbox
