#!/usr/bin/env bash
# Build a relocatable tarball: binaries, SDK, analysis layer and the docs that explain them.
#
#   bash tools/pack.sh            # -> dist/linux-sandbox-<version>-linux-<arch>.tar.gz
#
# It reuses tools/install.sh into a staging directory, so what ships is exactly what an
# install produces (no second copy of the layout rules).
set -u -o pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
fail() { echo "FAIL: $*" >&2; exit 1; }

version=$(sed -n 's/^project(linux_sandbox VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || fail "cannot read the version from CMakeLists.txt"
arch=$(uname -m)
name="linux-sandbox-$version-linux-$arch"
stage="$root/dist/$name"

[ -x build/sandbox ] || fail "build first: cmake -S . -B build && cmake --build build"
rm -rf "$stage"
mkdir -p "$stage" || fail "cannot create $stage"

bash tools/install.sh --prefix "$stage" >/dev/null || fail "staging install failed"

# Documentation and licence travel with the binaries: a tarball without them is not usable.
install -m 0644 LICENSE CHANGELOG.md README.md "$stage/" || fail "copying top-level docs"
mkdir -p "$stage/docs"
install -m 0644 docs/*.md "$stage/docs/" || fail "copying docs"

tar -C "$root/dist" -czf "$root/dist/$name.tar.gz" "$name" || fail "tar failed"
( cd "$root/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" ) || fail "sha256 failed"

echo "== packed"
echo "   archive : dist/$name.tar.gz"
echo "   checksum: dist/$name.tar.gz.sha256"
echo "   unpack  : tar -xzf dist/$name.tar.gz -C /opt   # /opt/$name/bin/sandbox"
echo "   check   : /opt/$name/bin/sandbox --version && /opt/$name/bin/sandbox doctor"
