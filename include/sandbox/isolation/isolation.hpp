#pragma once
#include "sandbox/model/config.hpp"
#include "sandbox/core/result.hpp"
#include "sandbox/platform/linux.hpp"
namespace sandbox {
class RootfsBuilder {
  public:
    static void build(const RunConfig &, const fs::path &mountpoint);
};
class SecurityPolicy {
  public:
    static void drop_capabilities();
    static void reset_target_signals();
    static void install_baseline();
};

// The policy `install_baseline()` imposes, identified by name *and* rule-set
// version. Two runs that both report "baseline" are only comparable when the
// version matches, so the version has to travel with the result.
[[nodiscard]] SecurityPolicyFacts security_policy_id(const RunConfig &);
// P2 only: descriptors 0/1/2 (target I/O), 5 control, 6 runtime, 7+ sources.
int init_main(const RunConfig &, const fs::path &root_mountpoint);
} // namespace sandbox
