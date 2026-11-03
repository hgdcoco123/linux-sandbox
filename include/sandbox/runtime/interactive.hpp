#pragma once
#include "sandbox/model/config.hpp"
namespace sandbox {
Json serve_session(RunConfig, const fs::path &state, const fs::path &launcher);
}
