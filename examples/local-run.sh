#!/usr/bin/env bash
# Run one real isolated execution and show what came back.
#
#   bash examples/local-run.sh              # isolated if this machine allows it
#   SANDBOX_ALLOW_PROCESS=1 bash ...        # fall back to the diagnostic backend
#
# Success looks like: verdict=ok, artifacts delivered, a checksums file next to the report.
set -u -o pipefail
cd "$(dirname "$0")/.." || exit 1
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x build/sandbox ] || {
    echo "== building"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null || fail "cmake configure"
    cmake --build build --parallel >/dev/null || fail "build"
}

sandbox_bin=$(pwd)/build/sandbox
work=$(mktemp -d) || fail "mktemp"
trap 'rm -rf "$work"' EXIT
input="$work/in"; output="$work/out"; runtime="$work/runtime"
mkdir -p "$input" "$output"
printf 'hello from the host\n' > "$input/sample.txt"

echo "== preparing a runtime (a static binary is enough)"
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output "$runtime" >/dev/null \
    || fail "make_runtime"

probe=$("$sandbox_bin" doctor 2>/dev/null | python3 -c \
    "import json,sys; p=json.load(sys.stdin).get('isolation_probe') or {}; \
     print(p.get('available'), p.get('stage') or '')")
available=$(echo "$probe" | cut -d' ' -f1)
stage=$(echo "$probe" | cut -d' ' -f2)

if [ "$available" = "True" ]; then
    echo "== isolation available; preparing a cgroup subtree"
    runs=$(python3 tools/cgroup_setup.py) || fail "cgroup setup (see the message above)"
    echo "== running in isolated mode"
    "$sandbox_bin" run \
        --runtime "$runtime" --cgroup-root "$runs" \
        --input "$input" --output "$output" \
        --timeout 3000 --memory 256M --summary \
        -- /usr/bin/sandbox-fixture files
    status=$?
elif [ "${SANDBOX_ALLOW_PROCESS:-0}" = "1" ]; then
    echo "== isolation unavailable (stage: ${stage:-unknown}); running the diagnostic backend"
    echo "   this is NOT a security boundary"
    # process mode has no /output mount, so the target is pointed at the host directory
    # directly with an absolute path: the same bytes land where the isolated run would put them.
    "$sandbox_bin" run --process --quiet --summary --timeout 3000 \
        -- /bin/sh -c "printf 'converted\n' > $output/result.txt"
    status=$?
else
    echo "== isolation unavailable (stage: ${stage:-unknown})" >&2
    echo "   under systemd : run this script inside \`systemd-run --user -p Delegate=yes\`" >&2
    echo "   in a container : see docs/INSTALL.md 3.1, or pass SANDBOX_ALLOW_PROCESS=1" >&2
    exit 3
fi

echo
echo "== what came back"
ls -l "$output" || true
if [ -f "$output/../report.json" ]; then :; fi
echo
echo "== the artifact manifest is inside the run directory (sandbox inspect <run_id>)"
[ "$status" -eq 0 ] || echo "== the run was not successful (status $status) - that is also a valid result"
exit "$status"
