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

build_dir="${SBX_BUILD_DIR:-$root/build}"
[ -x "$build_dir/sandbox" ] || fail "build first: cmake -S . -B build && cmake --build build"
rm -rf "$stage"
mkdir -p "$stage" || fail "cannot create $stage"

SBX_BUILD_DIR="$build_dir" bash tools/install.sh --prefix "$stage" >/dev/null || fail "staging install failed"

# A tarball that needs libprotobuf.so.32 and libseccomp.so.2 breaks on the next distro. Report
# the truth about the binaries being shipped instead of leaving it to the first user.
external=$(ldd "$build_dir/sandbox" 2>/dev/null | grep -E "libprotobuf|libseccomp" | awk "{print \$1}" | tr "\n" " ")
if [ -n "$external" ]; then
    echo "note: these binaries need system libraries: $external" >&2
    echo "      for a portable tarball rebuild with -DSANDBOX_STATIC_DEPS=ON" >&2
fi

# Documentation and licence travel with the binaries: a tarball without them is not usable.
install -m 0644 LICENSE CHANGELOG.md README.md "$stage/" || fail "copying top-level docs"
# The tarball installs itself: no git, no network, no script to fetch separately.
install -m 0755 tools/package-install.sh "$stage/install.sh" || fail "copying install.sh"
mkdir -p "$stage/docs"
install -m 0644 docs/*.md "$stage/docs/" || fail "copying docs"

tar -C "$root/dist" -czf "$root/dist/$name.tar.gz" "$name" || fail "tar failed"
( cd "$root/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" ) || fail "sha256 failed"

echo "== packed"
echo "   archive : dist/$name.tar.gz"
echo "   checksum: dist/$name.tar.gz.sha256"
echo "   unpack  : tar -xzf dist/$name.tar.gz -C /opt   # /opt/$name/bin/sandbox"
echo "   check   : /opt/$name/bin/sandbox --version && /opt/$name/bin/sandbox doctor"
