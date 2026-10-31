#pragma once
#include "sandbox/config.hpp"
#include "sandbox/linux.hpp"
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
// P2 only: descriptors 0/1/2 (target I/O), 5 control, 6 runtime, 7+ sources.
int init_main(const RunConfig &, const fs::path &root_mountpoint);
} // namespace sandbox
