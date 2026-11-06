#!/usr/bin/env bash
# End-to-end check of the controlled verification loop on a real isolated sandbox.
#
# Run as yourself, NOT with sudo: the script asks for sudo only to relax and restore the
# host sysctl, exactly like tools/isolated-accept.sh. Running it as root would skip the
# delegated launch path this is meant to exercise (sdk.launch refuses delegated startup
# for root by design).
#
# What it proves: a real isolated run is imported, an analysis proposes a registered
# template, the operator path executes it for real, the receipt is recorded and reaches
# the next turn, and citing the verification run's evidence passes validation.
#
# --dry-run validates the harness (configs, registry, digest) without touching the sandbox
# or the host policy, so it works on a machine that cannot do isolation.
set -u -o pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

sysctl_key=kernel.apparmor_restrict_unprivileged_userns
binary="$root/build/sandbox"
fixture="$root/build/sandbox-fixture"
dry_run=0
[ "${1:-}" = "--dry-run" ] && dry_run=1

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

[ "$(id -u)" -ne 0 ] ||
    fail "run this as your own user, not with sudo: it uses sudo itself, and root would skip the delegated launch path"

[ -x "$binary" ] || fail "no $binary: build the tree first (cmake --build build)"
[ -x "$fixture" ] || fail "no $fixture: build the tree first"

work=$(mktemp -d /tmp/sbx-ai-verify.XXXXXX) || fail "cannot create a work directory"
runtime="$work/runtime"
python3 tools/make_runtime.py --fixture "$fixture" --output "$runtime" >/dev/null ||
    fail "cannot build the test runtime"

if [ "$dry_run" -eq 1 ]; then
    python3 tools/ai_verify_e2e.py --sandbox "$binary" --runtime "$runtime" \
        --work "$work" --skip-run || fail "dry run failed"
    echo "AI VERIFY E2E DRY RUN OK (no sandbox, no host policy change): work at $work"
    exit 0
fi

original=$(sysctl -n "$sysctl_key" 2>/dev/null) ||
    fail "cannot read $sysctl_key (needs a Linux host with that knob)"
echo "== $sysctl_key is currently $original"
sudo -v || fail "sudo is required to relax and restore $sysctl_key"

restore_failed=0
restore() {
    if sudo -n sysctl -q -w "$sysctl_key=$original"; then
        echo "== restored $sysctl_key=$original"
    else
        echo "== FAILED to restore $sysctl_key: the host is still relaxed" >&2
        echo "==   run manually: sudo sysctl -w $sysctl_key=$original" >&2
        restore_failed=1
    fi
}
trap 'restore; [ "$restore_failed" -eq 0 ] || exit 1' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

systemctl --user is-system-running >/dev/null 2>&1 ||
    fail "no running user systemd session; systemd-run --user needs one"
sudo -n sysctl -q -w "$sysctl_key=0" || fail "cannot relax $sysctl_key"
echo "== relaxed $sysctl_key=0 for the duration of this run"

log="$work/run.log"
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
    /usr/bin/python3 "$root/tools/ai_verify_e2e.py" \
    --sandbox "$binary" --runtime "$runtime" --work "$work" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

required=(
    "real isolated verification executed"
    "verification ran isolated and cleaned up"
    "verification receipt visible to the next turn"
    "verification evidence is citable"
    "verification target failure is recorded as target_failed"
    "verification target timeout is reported, not hidden"
    "a changed template is refused before anything runs"
)
incomplete=0
for line in "${required[@]}"; do
    if grep -qF "$line" "$log"; then
        echo "== ok: $line"
    else
        echo "== MISSING: $line" >&2
        incomplete=1
    fi
done
if grep -qF "FAIL:" "$log"; then
    echo "== the driver reported a failure" >&2
    incomplete=1
fi
if [ "$status" -ne 0 ]; then
    echo "== ai_verify_e2e.py exited $status" >&2
    incomplete=1
fi

echo "== work kept at $work (state, configs, receipt, stats)"
[ "$incomplete" -eq 0 ] || fail "verification loop did not close"
echo "AI VERIFY E2E PASSED: a real isolated verification closed the loop"
