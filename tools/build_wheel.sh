#!/usr/bin/env bash
# Build a wheels-only distribution that a stranger can install with `pip install linux-sandbox`.
#
#   bash tools/build_wheel.sh                    # uses build-static if present, else build
#   SBX_BUILD_DIR=build-static bash tools/build_wheel.sh
#
# The wheel is platform tagged (py3-none-manylinux_2_38_x86_64) and carries the statically linked
# `sandbox` and `sandbox-launcher`, with console scripts that exec them. Nothing reaches PyPI
# until somebody with a token runs the upload command this prints.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail() { echo "build_wheel: $*" >&2; exit 1; }

build_dir="${SBX_BUILD_DIR:-}"
if [ -z "$build_dir" ]; then
    if [ -x build-static/sandbox ]; then build_dir=build-static; else build_dir=build; fi
fi
[ -x "$build_dir/sandbox" ] || fail "no $build_dir/sandbox: build first"
external=$(ldd "$build_dir/sandbox" 2>/dev/null | grep -E "libprotobuf|libseccomp" | awk '{print $1}' | tr '\n' ' ' || true)
[ -z "$external" ] || fail "these binaries need system libraries ($external); rebuild with -DSANDBOX_STATIC_DEPS=ON"

for tool in sandbox_ai/__init__.py setup.py; do [ -f "$tool" ] || fail "missing $tool"; done

# The binaries and the metadata must describe the same release; a wheel whose `sandbox --version`
# disagrees with its own filename is the kind of mistake nobody notices until a bug report.
want=$(sed -n 's/^version = "\(.*\)"/\1/p' pyproject.toml | head -1)
have=$("$build_dir/sandbox" --version 2>/dev/null | awk '{print $2}')
[ "$want" = "$have" ] || fail "binaries are $have but pyproject says $want; rebuild $build_dir first"

stage=linux_sandbox_bin
cleanup() { rm -rf "$stage" build/bdist.* 2>/dev/null || true; }
trap cleanup EXIT

rm -rf "$stage" dist
mkdir -p "$stage"
cp "$build_dir/sandbox" "$build_dir/sandbox-launcher" "$stage/"
chmod 0755 "$stage"/*
: > "$stage/__init__.py"
echo "build_wheel: staged $stage/sandbox and $stage/sandbox-launcher"

python3 -m build --wheel --no-isolation --outdir dist >/tmp/build-wheel.log 2>&1 \
    || { tail -20 /tmp/build-wheel.log; fail "python -m build failed (need: pip install build wheel)"; }

wheel=$(ls dist/*.whl | head -1)
case "$wheel" in
    *py3-none-any*) fail "wheel is tagged pure python; setup.py's bdist_wheel override did not apply" ;;
esac

echo "build_wheel: $wheel"
python3 - "$wheel" <<'PY'
import sys, zipfile
wheel = sys.argv[1]
with zipfile.ZipFile(wheel) as zf:
    names = zf.namelist()
    bins = [n for n in names if n.startswith("linux_sandbox_bin/") and not n.endswith("__init__.py")]
    modes = {n: (zf.getinfo(n).external_attr >> 16) & 0o777 for n in bins}
print("   binaries:", ", ".join(b.split("/")[-1] for b in bins))
print("   modes   :", ", ".join(f"{b.split('/')[-1]}={oct(m)}" for b, m in modes.items()))
missing = [b for b in bins if not modes[b] & 0o111]
if missing:
    sys.exit("   FAIL: not executable inside the wheel: " + ", ".join(missing))
PY

cat <<EOF

next, to publish (needs an account on pypi.org and a token):
  python3 -m pip install --user twine
  python3 -m twine upload $wheel

after that, any user with a PyPI mirror can install it:
  pip install linux-sandbox        # gives sandbox, sbx and sandbox-ai
EOF
