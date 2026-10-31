#include "sandbox/init_common.hpp"
#include "sandbox/isolation.hpp"
#include <sys/prctl.h>

namespace sandbox {

void arm_init(bool isolated) {
    // A pid 1 that outlives its supervisor would keep the namespace alive with
    // nothing able to reap it.
    checked(::prctl(PR_SET_PDEATHSIG, SIGKILL), "init parent death");
    if (!isolated)
        checked(::prctl(PR_SET_CHILD_SUBREAPER, 1), "init subreaper");
}

void prepare_environment(const RunConfig &config, const fs::path &root) {
    if (config.mode == "isolated")
        RootfsBuilder::build(config, root);
    else
        close_from(6);
    SecurityPolicy::drop_capabilities();
    checked(::prctl(PR_SET_DUMPABLE, 0), "protect init");
    // Re-arm after the credential changes above, which can clear it.
    checked(::prctl(PR_SET_PDEATHSIG, SIGKILL), "init parent death after setup");
}

} // namespace sandbox
