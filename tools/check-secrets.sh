#!/usr/bin/env bash
# Refuse to ship credentials.
#
#   bash tools/check-secrets.sh            # tracked files + full history
#   bash tools/check-secrets.sh --fast     # tracked files only
#
# This exists because a token was once pasted into a shell and only cleaned up afterwards by
# hand. The check is deliberately narrow: it looks for credential-shaped strings, not for words
# like "token" in prose. Placeholders such as `pypi-XXXXXXXX` and `SANDBOX_AI_API_KEY=...` are
# below the length thresholds on purpose.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
fast=0
[ "${1:-}" = "--fast" ] && fast=1

# Each pattern is a credential format, not a topic. Keep them specific: a noisy check gets
# disabled, and a disabled check protects nothing.
pattern='sk-[A-Za-z0-9]{20,}'
pattern+='|AKIA[0-9A-Z]{16}'
pattern+='|BEGIN (RSA |OPENSSH |EC |DSA |PGP )?PRIVATE KEY'
pattern+='|gh[pousr]_[A-Za-z0-9]{30,}'
pattern+='|glpat-[A-Za-z0-9_-]{20,}'
pattern+='|xox[abposr]-[A-Za-z0-9-]{10,}'
pattern+='|(api[_-]?key|secret|password|token)[[:space:]]*[=:][[:space:]]*["'"'"'][A-Za-z0-9_\-]{24,}["'"'"']'

fail=0
# Scan the working tree, not just the index: a file about to be committed is exactly what matters,
# and `git grep` alone silently ignores untracked files.
echo "check-secrets: working tree"
files=$(git ls-files -co --exclude-standard 2>/dev/null | grep -vE '^tools/check-secrets\.sh$' || true)
if [ -n "$files" ]; then
    hits=$(printf '%s\n' "$files" | xargs -r -d '\n' grep -InE "$pattern" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | sed 's/^/  /'
        fail=1
    fi
fi

if [ "$fast" -eq 0 ]; then
    echo "check-secrets: history (all commits)"
    # Scan every commit's tree once per commit; small repositories make this cheap, and it is the
    # only way to catch a secret that was committed and then deleted.
    hist=$(git rev-list --all | while read -r rev; do
        git grep -InE "$pattern" "$rev" -- . 2>/dev/null | grep -v '^tools/check-secrets.sh:'
    done | head -20 || true)
    if [ -n "$hist" ]; then
        printf '%s\n' "$hist" | sed 's/^/  /'
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    cat >&2 <<'EOF'

check-secrets: credential-shaped strings found.
  - if it is real: revoke it first, then remove it from the tree and from history
  - if it is a test fixture: use an obviously fake value below the length thresholds
EOF
    exit 1
fi
echo "check-secrets: clean"
