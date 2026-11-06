#!/usr/bin/env bash
# Build and test this checkout. One command, output on the terminal.
#
#   bash verify.sh            # configure, build, run all tests
#   bash verify.sh --clean    # start from an empty build directory
#   bash verify.sh --ctest    # skip the build, only run the tests
#
# Exit code is 0 only when every test passes.
set -u

root=$(cd "$(dirname "$0")" && pwd)
cd "$root" || exit 1

clean=0
skip_build=0
for arg in "$@"; do
    case "$arg" in
    --clean) clean=1 ;;
    --ctest) skip_build=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [ "$clean" = 1 ]; then
    echo "== removing build/"
    rm -rf build
fi

if [ "$skip_build" = 0 ]; then
    echo "== configure"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug || exit 1
    echo "== build"
    cmake --build build --parallel 2 || {
        echo
        echo "BUILD FAILED. Last 40 lines of the build log:"
        cmake --build build --parallel 2 2>&1 | tail -40
        exit 1
    }
fi

echo "== test"
ctest --test-dir build --output-on-failure
status=$?

# The analysis layer's contracts are a CTest case above; this covers the part CTest cannot:
# the offline demo and its aggregate metrics, on any host with Python 3.
if [ "$status" = 0 ] && command -v python3 >/dev/null 2>&1; then
    echo "== ai demo"
    bash tools/ai-check.sh || status=1
fi

echo
if [ "$status" = 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED (exit $status) - the failing cases are listed above"
    echo "note: isolation_integration is expected to be SKIPPED without a delegated cgroup"
fi
exit "$status"
