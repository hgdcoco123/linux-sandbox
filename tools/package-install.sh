#!/bin/sh
# Inside an unpacked release tarball: link the entry points, prove the binaries run, report
# whether this host can isolate. No network, no git, no root.
#
#   sh install.sh              # link into ~/.local/bin and smoke test
#   sh install.sh /opt/prefix  # link into another prefix
#
# The package layout is assumed relative to this file, so it can be unpacked anywhere.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${1:-$HOME/.local}
bindir="$prefix/bin"

[ -x "$here/bin/sandbox" ] || { echo "install: $here does not look like a package (no bin/sandbox)" >&2; exit 1; }

mkdir -p "$bindir" 2>/dev/null || { echo "install: cannot create $bindir; pass a writable prefix" >&2; exit 1; }
# `sbx` is an alias for `sandbox`: on Ubuntu the name `sandbox` is also provided by
# policycoreutils-python-utils (SELinux), and a user who installs that gets a different program.
for exe in sandbox sandbox-launcher sandbox-ai; do
    [ -e "$here/bin/$exe" ] && ln -sfn "$here/bin/$exe" "$bindir/$exe"
done
[ -e "$here/bin/sandbox" ] && ln -sfn "$here/bin/sandbox" "$bindir/sbx"

echo "install: linked into $bindir"
"$here/bin/sandbox" --version || { echo "install: the binary does not start on this system" >&2; exit 1; }

set +e
"$here/bin/sandbox" run --process --quiet --summary -- /bin/sh -c 'echo install-ok; exit 3' >/dev/null 2>&1
status=$?
set -e
if [ "$status" -ne 10 ]; then
    echo "install: smoke run returned $status, expected 10 (target_failed for 'exit 3')" >&2
    echo "         run: $here/bin/sandbox doctor" >&2
    exit 1
fi
echo "install: smoke run ok (10 = target_failed, the expected verdict for 'exit 3')"

json=$(mktemp 2>/dev/null || echo /tmp/.sbx-doctor.json)
"$here/bin/sandbox" doctor >"$json" 2>/dev/null || true
if [ -f "$json" ] && grep -q '"available": true' "$json"; then
    echo "install: isolated execution available"
else
    stage=$(grep -o '"stage": "[^"]*"' "$json" 2>/dev/null | head -1 | cut -d'"' -f4)
    echo "install: isolated execution NOT available${stage:+ (stage: $stage)}"
    echo "         see $here/docs/INSTALL.md section 3.0; the diagnostic backend still works:"
    echo "           sandbox run --process --quiet --summary -- /bin/sh -c 'echo hi; exit 3'"
fi

case ":${PATH}:" in
    *":$bindir:"*) ;;
    *) echo "install: add this once, then open a new shell:"
       echo "         echo 'export PATH=\"$bindir:\$PATH\"' >> ~/.bashrc" ;;
esac
