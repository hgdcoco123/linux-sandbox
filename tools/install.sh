#!/usr/bin/env bash
# Install the sandbox binaries, SDK and helper scripts into a prefix.
#
#   bash tools/install.sh                 # ~/.local
#   bash tools/install.sh --prefix /opt/linux-sandbox   # needs write access
#
# Nothing is installed outside the prefix, no service is enabled, no sudo is used.
# Re-running is safe: files are replaced in place.
set -u -o pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
prefix="${HOME}/.local"
while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) prefix="${2:?--prefix needs a path}"; shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }

build_dir="${SBX_BUILD_DIR:-$root/build}"
binary="$build_dir/sandbox"
launcher="$build_dir/sandbox-launcher"
[ -x "$binary" ] || fail "no $binary: build first (cmake --build build)"
[ -x "$launcher" ] || fail "no $launcher: build first"
[ -w "$prefix" ] || [ -w "$(dirname "$prefix")" ] || fail "cannot write to $prefix"

mkdir -p "$prefix/bin" "$prefix/share/linux-sandbox/sdk" \
         "$prefix/share/linux-sandbox/tools" \
         "$prefix/share/linux-sandbox/sandbox_ai" || fail "cannot create $prefix layout"

install -m 0755 "$binary" "$prefix/bin/sandbox" || fail "copying sandbox"
# Same binary under a name that cannot collide with Ubuntu's SELinux `sandbox`
# (policycoreutils-python-utils provides one).
ln -sfn sandbox "$prefix/bin/sbx" || fail "linking sbx"
# The CLI resolves the launcher next to itself, so it must live in the same directory.
install -m 0755 "$launcher" "$prefix/bin/sandbox-launcher" || fail "copying sandbox-launcher"
install -m 0644 "$root"/sdk/*.py "$prefix/share/linux-sandbox/sdk/" || fail "copying the SDK"
for helper in delegated_run.py delegated_cgroup.py make_runtime.py; do
    [ -f "$root/tools/$helper" ] || fail "missing tools/$helper"
    install -m 0755 "$root/tools/$helper" "$prefix/share/linux-sandbox/tools/" || fail "copying $helper"
done
# The analysis layer is part of the product, not a repository-only script: without this the
# installed tree cannot explain a run at all.
[ -d "$root/sandbox_ai" ] || fail "missing sandbox_ai package"
install -m 0644 "$root"/sandbox_ai/*.py "$prefix/share/linux-sandbox/sandbox_ai/" \
    || fail "copying sandbox_ai"
# Relocatable on purpose: the same wrapper has to work from a prefix and from an unpacked
# release tarball, so the package root is derived from the script's own location.
cat > "$prefix/bin/sandbox-ai" <<'WRAPPER' || fail "writing sandbox-ai"
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHONPATH="$here/../share/linux-sandbox${PYTHONPATH:+:$PYTHONPATH}" exec python3 -m sandbox_ai "$@"
WRAPPER
chmod 0755 "$prefix/bin/sandbox-ai" || fail "chmod sandbox-ai"

cat <<TEXT
== installed
   binaries : $prefix/bin/sandbox, $prefix/bin/sandbox-launcher
   explain  : $prefix/bin/sandbox-ai        # same CLI as python -m sandbox_ai
   sdk      : $prefix/share/linux-sandbox/sdk
   helpers  : $prefix/share/linux-sandbox/tools
== next
   1. put $prefix/bin on PATH (if it is not already)
   2. $prefix/bin/sandbox doctor        # reports which startup modes work here
   3. $prefix/bin/sandbox run --summary -- /bin/echo hello
   4. read docs/INSTALL.md for what "isolated" needs beyond a working binary
TEXT
