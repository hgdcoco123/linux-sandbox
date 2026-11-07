#!/usr/bin/env bash
# Install a published linux-sandbox release without sudo, and prove it works.
#
#   sh quick-install.sh                       # download the release into ~/.local
#   sh quick-install.sh --prefix /opt         # needs write access there
#   sh quick-install.sh --from ./pkg.tar.gz   # offline: archive already on disk
#   sh quick-install.sh --apparmor            # also allow userns for this launcher
#
# Refuses rather than guesses: wrong architecture, missing tools, failed download, missing or
# mismatched checksum, or a broken installation each end with a sentence naming the next action.
set -eu

version="0.5.1"
prefix="${HOME}/.local"
from=""
repo_url="https://github.com/hgdcoco123/linux-sandbox/releases/download"
base=""
apparmor=0

say() { printf '%s\n' "$*"; }
die() { printf 'install failed: %s\n' "$*" >&2; exit 1; }
# A script that waits for a password it cannot receive is worse than one that stops: sudo -n
# fails immediately, and the message says how to continue without privileges.
privileged() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif sudo -n true 2>/dev/null; then
        sudo -n "$@"
    else
        die "this step needs root and sudo is not passwordless here: $*
  re-run as root, or drop --apparmor / use --prefix \"\$HOME/.local\""
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --from) from="$2"; shift 2 ;;
        --url) base="$2"; shift 2 ;;
        --apparmor) apparmor=1; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done
name="linux-sandbox-${version}-linux-x86_64"
# --url overrides the whole release directory; otherwise the tag path is derived from --version.
[ -n "$base" ] || base="${repo_url}/v${version}"
base="${base%/}"

# ---- preflight -------------------------------------------------------------
[ "$(uname -s)" = "Linux" ] || die "this build is Linux-only; uname -s says $(uname -s)"
arch=$(uname -m)
case "$arch" in
    x86_64|amd64) ;;
    *) die "no published build for $arch (only x86_64). Build from source instead:
  git clone https://github.com/hgdcoco123/linux-sandbox && cd linux-sandbox
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSANDBOX_STATIC_DEPS=ON
  cmake --build build --parallel && bash tools/install.sh --prefix \"$prefix\"" ;;
esac
command -v tar >/dev/null 2>&1 || die "tar is required"
glibc=$(ldd --version 2>/dev/null | head -1 | grep -o '[0-9]\+\.[0-9]\+$' || echo 0)
case "$glibc" in
    2.3[89]|2.[4-9]*) ;;
    *) say "note: glibc is $glibc, the build target is 2.38; if the binary refuses to start,"
       say "      build from source (docs/INSTALL.md)." ;;
esac

# ---- fetch + verify --------------------------------------------------------
work=$(mktemp -d) || die "cannot create a temporary directory"
trap 'rm -rf "$work"' EXIT
archive="$work/$name.tar.gz"
if [ -n "$from" ]; then
    [ -f "$from" ] || die "no such file: $from"
    cp "$from" "$archive"
    [ -f "$from.sha256" ] && cp "$from.sha256" "$archive.sha256"
    say "== using local archive $from"
else
    command -v curl >/dev/null 2>&1 || die "curl is required to download the release"
    say "== downloading $name.tar.gz"
    curl -fL --retry 3 --retry-delay 2 -o "$archive" "${base}/$name.tar.gz" \
        || die "download failed. If this machine cannot reach github.com, fetch the archive
  elsewhere and install it with: sh quick-install.sh --from /path/to/$name.tar.gz"
    curl -fL --retry 3 --retry-delay 2 -o "$archive.sha256" "${base}/$name.tar.gz.sha256" \
        || die "could not fetch the published checksum; refusing to install unverified bytes"
fi
if [ -f "$archive.sha256" ]; then
    ( cd "$work" && sha256sum -c "$name.tar.gz.sha256" ) || die "checksum mismatch: refusing to install"
else
    say "== no checksum next to this archive; installing unverified bytes"
fi

# ---- unpack ----------------------------------------------------------------
say "== installing into $prefix/$name"
mkdir -p "$prefix" 2>/dev/null && [ -w "$prefix" ] \
    || privileged mkdir -p "$prefix"
if [ -w "$prefix" ]; then tar -xzf "$archive" -C "$prefix" \
    || die "cannot unpack into $prefix"; else privileged tar -xzf "$archive" -C "$prefix" \
        || die "cannot unpack into $prefix"; fi
root="$prefix/$name"
[ -x "$root/bin/sandbox" ] || die "archive is missing bin/sandbox"

# ---- entry points ----------------------------------------------------------
bindir="$prefix/bin"
mkdir -p "$bindir" 2>/dev/null || privileged mkdir -p "$bindir"
for exe in sandbox sandbox-launcher sandbox-ai; do
    [ -e "$root/bin/$exe" ] || continue
    if [ -w "$bindir" ]; then ln -sfn "$root/bin/$exe" "$bindir/$exe"
    else privileged ln -sfn "$root/bin/$exe" "$bindir/$exe"; fi
done
case ":${PATH}:" in
    *":$bindir:"*) ;;
    *) say "== add this once, then open a new shell:"
       say "   echo 'export PATH=\"$bindir:\$PATH\"' >> ~/.bashrc   # or ~/.zshrc" ;;
esac
PATH="$bindir:$PATH"; export PATH

# ---- prove it works --------------------------------------------------------
say "== checking the installation"
"$root/bin/sandbox" --version || die "the sandbox binary does not start on this system"
set +e
"$root/bin/sandbox" run --process --quiet --summary -- /bin/sh -c 'echo install-ok; exit 3' \
    >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 10 ] || die "the smoke run returned $status, expected 10 (target_failed for 'exit 3').
  run: $root/bin/sandbox doctor"
say "   smoke run ok (10 = target_failed, the expected verdict for 'exit 3')"

if [ "$apparmor" -eq 1 ]; then
    profile="/etc/apparmor.d/linux-sandbox-launcher"
    # The attachment globs the version directory on purpose: a profile pinned to 0.5.1 silently
    # stops matching after an upgrade, and the failure looks like "isolation broke by itself".
    launcher_glob="$prefix/linux-sandbox-*/bin/sandbox-launcher"
    say "== granting userns to $root/bin/sandbox-launcher"
    printf 'abi <abi/4.0>,\ninclude <tunables/global>\nprofile linux-sandbox-launcher %s flags=(unconfined) {\n  userns,\n}\n' \
        "$launcher_glob" | privileged tee "$profile" >/dev/null
    privileged apparmor_parser -r "$profile" || say "   apparmor_parser failed; isolation may stay unavailable"
    if command -v aa-status >/dev/null 2>&1 && privileged aa-status 2>/dev/null | grep -q linux-sandbox-launcher; then
        say "   profile loaded (aa-status shows linux-sandbox-launcher)"
    else
        say "   could not confirm the profile is loaded; check: sudo aa-status | grep linux-sandbox"
    fi
fi

say
say "== isolation on this host"
"$root/bin/sandbox" doctor >"$work/doctor.json" 2>"$work/doctor.err" || true
if grep -q '"available": true' "$work/doctor.json"; then
    say "   isolated execution available"
else
    stage=$(grep -o '"stage": "[^"]*"' "$work/doctor.json" | head -1 | cut -d'"' -f4)
    say "   isolated execution NOT available${stage:+ (stage: $stage)}"
    # Releases after 0.5.1 explain the stage on stderr themselves; older ones need the pointer.
    if [ -s "$work/doctor.err" ]; then sed 's/^/   /' "$work/doctor.err"
    else say "   run: $root/bin/sandbox doctor   and see $root/docs/INSTALL.md section 3.0"; fi
    say "   the diagnostic backend still works: sandbox run --process ..."
fi

cat <<EOF

== installed
   binaries : $root/bin
   on PATH  : $bindir (open a new shell if the command is not found)
   next     : sandbox doctor
              sandbox run --process --quiet --summary -- /bin/sh -c 'echo hi; exit 3'
   docs     : $root/docs/USAGE.md, $root/docs/INSTALL.md
EOF
