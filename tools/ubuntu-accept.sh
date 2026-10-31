#!/usr/bin/env bash
# Collect everything needed to diagnose a first run on a real Linux host.
#
#   bash tools/ubuntu-accept.sh            # build, test, collect
#   bash tools/ubuntu-accept.sh --skip-iso # skip the delegated isolation run
#
# The script never stops at the first failure: a partial report is worth more
# than none. It does not modify the host (no sudo installs, no sysctl, no
# AppArmor changes).
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/acceptance-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
log() { printf '%s\n' "$*" | tee -a "$out/summary.txt"; }
run() { # run <name> <command...>: capture output, show it live, record the code
    local name=$1; shift
    printf '\n== %s\n' "$name"
    "$@" 2>&1 | tee -a "$out/$name.txt"
    local code=${PIPESTATUS[0]}
    printf '%-24s exit=%s\n' "$name" "$code" | tee -a "$out/summary.txt"
    return 0
}

log "root: $root"
log "date: $(date -Is)"
if [ -d "$root/.git" ]; then
    log "commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null) $(git -C "$root" log -1 --format=%s 2>/dev/null)"
    if [ -n "$(git -C "$root" status --porcelain 2>/dev/null)" ]; then
        log "warning: working tree has uncommitted changes"
    fi
else
    log "commit: (not a git checkout - copy the repository for reproducible results)"
fi
if command -v sha256sum >/dev/null 2>&1; then
    stamp=$(cat CMakeLists.txt src/core/ops_json.cpp src/runtime/run_session.cpp 2>/dev/null |
        sha256sum | cut -c1-12)
    log "source stamp: $stamp"
fi
{
    uname -a
    cat /etc/os-release
    systemctl --version | head -1
    cat /proc/self/cgroup
    id
} >"$out/host.txt" 2>&1

missing=""
for tool in cmake g++ protoc python3; do
    command -v "$tool" >/dev/null || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    log "MISSING TOOLS:$missing"
    log "install with: sudo apt-get install -y build-essential cmake libseccomp-dev protobuf-compiler libprotobuf-dev python3-protobuf"
fi

cd "$root" || exit 1
run configure cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
run build cmake --build build --parallel 2
run ctest ctest --test-dir build --output-on-failure

# Print the essentials to the terminal so they can be copied straight into a
# chat without opening any files. Everything is still saved in $out.
printf '\n===== BUILD (tail) =====\n'
tail -n 20 "$out/build.txt" 2>/dev/null

printf '\n===== SUMMARY =====\n'
cat "$out/summary.txt"
if ! grep -q '^build .*exit=0' "$out/summary.txt"; then
    printf '\n===== BUILD FAILED (tail 60) =====\n'
    tail -n 60 "$out/build.txt" 2>/dev/null
fi
if ! grep -q '^ctest .*exit=0' "$out/summary.txt"; then
    printf '\n===== TEST FAILURES =====\n'
    grep -E 'Failed|\*\*\*|FAILED|Errors while|tests passed|tests failed' "$out/ctest.txt" 2>/dev/null | head -60
    printf '\n----- tail of ctest output -----\n'
    tail -n 60 "$out/ctest.txt" 2>/dev/null
fi
printf '\n===== END =====\n'

if [ "${1:-}" != "--skip-iso" ] && [ -x build/sandbox ] && [ -f build/sandbox-fixture ]; then
    test_root=$(mktemp -d /tmp/sbx-acc.XXXXXX)
    run fixture python3 tools/make_runtime.py --fixture build/sandbox-fixture \
        --output "$test_root/runtime"
    if systemd-run --user --version >/dev/null 2>&1; then
        run isolation systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
            /usr/bin/python3 "$root/tools/delegated_run.py" \
            --sandbox "$root/build/sandbox" --test-runtime "$test_root/runtime"
    else
        log "isolation               skipped (no user systemd-run)"
    fi
fi

# State left behind by a failing run is the most useful artifact.
uid=$(id -u)
if [ -d "/tmp/sbx-$uid" ]; then
    cp -r "/tmp/sbx-$uid" "$out/state" 2>/dev/null
fi
ls /sys/fs/cgroup | grep -E '^sbx-' >"$out/cgroup-leftovers.txt" 2>&1
{
    dmesg 2>/dev/null | tail -40
    journalctl --user -n 50 --no-pager 2>/dev/null | tail -50
    aa-status 2>/dev/null | head -20
} >"$out/kernel.txt" 2>&1

archive="$out.tar.gz"
tar -czf "$archive" -C "$(dirname "$out")" "$(basename "$out")" 2>/dev/null

log "---"
log "archive: $archive"
log "send: summary.txt, build.txt (if it failed), ctest.txt, isolation.txt (if it ran), kernel.txt"
log "state/ and cgroup-leftovers.txt only matter when something failed"
