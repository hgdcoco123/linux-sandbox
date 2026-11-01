#!/usr/bin/env bash
# Isolated acceptance, with the host policy relaxed for exactly as long as this runs.
#
# The relaxation is a host decision, not a project default: this script records the
# current value, restores *that* value on exit (success, failure or interrupt), and
# never assumes it started at 1.
#
# Skipped cases are failures. `isolation_integration` prints SKIP and exits 77 when
# there is no delegated cgroup; that is correct behaviour for the suite but not
# evidence, so this script requires the individual success lines to appear.
set -u -o pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

sysctl_key=kernel.apparmor_restrict_unprivileged_userns
binary="$root/build/sandbox"
fixture="$root/build/sandbox-fixture"
test_root=$(mktemp -d /tmp/sbx-acc.XXXXXX)

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

original=$(cat "/proc/sys/$sysctl_key" 2>/dev/null) ||
    fail "cannot read /proc/sys/$sysctl_key (needs a Linux host with that knob)"
echo "== $sysctl_key is currently $original"

# Ask for the password once, up front: the restore trap must not block on a prompt.
sudo -v || fail "sudo is required to relax and restore $sysctl_key"
restore() {
    sudo -n sysctl -q -w "$sysctl_key=$original"
    echo "== restored $sysctl_key=$original"
}
trap restore EXIT INT TERM

[ -x "$binary" ] || fail "no $binary: build the tree first (cmake --build build)"
[ -x "$fixture" ] || fail "no $fixture: build the tree first"
systemctl --user is-system-running >/dev/null 2>&1 ||
    fail "no running user systemd session; systemd-run --user needs one"

runtime="$test_root/runtime"
python3 tools/make_runtime.py --fixture "$fixture" --output "$runtime" >/dev/null ||
    fail "cannot build the test runtime"

sudo -n sysctl -q -w "$sysctl_key=0" || fail "cannot relax $sysctl_key"
echo "== relaxed $sysctl_key=0 for the duration of this run"

output="$test_root/run.log"
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
    /usr/bin/python3 "$root/tools/delegated_run.py" \
    --sandbox "$binary" --test-runtime "$runtime" 2>&1 | tee "$output"
status=${PIPESTATUS[0]}

# Every case the design requires must actually have run and passed.
required=(
    "isolated integration passed"
    "process sessions and artifact regression passed"
    "isolated session smoke passed"
    "isolated /input snapshot passed"
    "workspace quota, export and failure reporting passed"
    "isolated batch smoke passed"
)
incomplete=0
for line in "${required[@]}"; do
    if grep -qF "$line" "$output"; then
        echo "== ok: $line"
    else
        echo "== MISSING: $line" >&2
        incomplete=1
    fi
done
if grep -qF "NOT EXECUTED" "$output"; then
    echo "== MISSING: a case reported NOT EXECUTED (skipping is not passing)" >&2
    incomplete=1
fi
if [ "$status" -ne 0 ]; then
    echo "== delegated_run.py exited $status" >&2
    incomplete=1
fi

echo "== log kept at $output (runtime at $runtime)"
[ "$incomplete" -eq 0 ] || fail "isolated acceptance incomplete"
echo "ISOLATED ACCEPTANCE PASSED: every required case ran and passed"
