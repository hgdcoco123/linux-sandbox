#!/usr/bin/env bash
# Mirror this repository to Gitee: branches, tags, and (optionally) a release with the tarball
# attached, so users who cannot reach GitHub still get source and a verified archive.
#
#   bash tools/mirror_to_gitee.sh --remote git@gitee.com:<user>/linux-sandbox.git
#   bash tools/mirror_to_gitee.sh --remote ... --token <GITEE_TOKEN> --release v0.5.3
#   bash tools/mirror_to_gitee.sh --remote ... --dry-run
#
# What a mirror does NOT give you: `uses: <owner>/<repo>@v1` in GitHub Actions only resolves on
# GitHub, so the composite action stays a GitHub feature. Everything else (source, docs, release
# archive, checksum) works the same.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

remote=""
token=""
release=""
dry_run=0
fail() { echo "mirror: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --remote) remote="$2"; shift 2 ;;
        --token) token="$2"; shift 2 ;;
        --release) release="$2"; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown option: $1 (try --help)" ;;
    esac
done

[ -n "$remote" ] || fail "--remote is required, e.g. git@gitee.com:you/linux-sandbox.git"
[ -d .git ] || fail "run this inside the repository"
case "$remote" in
    *gitee.com*) ;;
    *) fail "this remote does not look like Gitee: $remote" ;;
esac

# owner/repo from either ssh (git@gitee.com:owner/repo.git) or https form
slug=$(printf '%s' "$remote" | sed -E 's#.*gitee\.com[:/]##; s#\.git$##; s#/$##')
[ -n "$slug" ] || fail "cannot derive owner/repo from $remote"

git fetch -q origin 2>/dev/null || true
version=$(sed -n 's/^version = "\(.*\)"/\1/p' pyproject.toml | head -1)
asset="linux-sandbox-${version}-linux-x86_64.tar.gz"

run() {
    if [ "$dry_run" -eq 1 ]; then echo "   [dry-run] $*"; else "$@"; fi
}

echo "== mirror: $slug (version $version)"
echo "== push branches and tags"
if ! run git push --prune "$remote" "+refs/heads/*:refs/heads/*" "+refs/tags/*:refs/tags/*"; then
    cat >&2 <<EOF

mirror: push was refused. Two ways forward:

  1) register this machine's key on Gitee (then re-run this script):
     https://gitee.com/profile/sshkeys
     paste the line printed by:  cat ~/.ssh/id_ed25519.pub

  2) push over HTTPS with a token instead of a key:
     bash tools/mirror_to_gitee.sh --remote https://<username>:<GITEE_TOKEN>@gitee.com/$slug.git
     (token from https://gitee.com/profile/personal_access_tokens, scope: projects)
EOF
    exit 1
fi

if [ -z "$release" ]; then
    cat <<EOF

== next: create the release on Gitee (web UI, 2 minutes)
   https://gitee.com/$slug/releases/new?tag_name=v$version
   title: v$version
   body : copy from docs/releases/v$version.md
   attach: ${asset} and ${asset}.sha256

   or re-run with --token <GITEE_TOKEN> --release v$version to do it through the API.
EOF
    exit 0
fi

[ -n "$token" ] || fail "--release needs --token (Gitee 私人令牌，勾选 projects 权限)"
archive="dist/$asset"
[ -f "$archive" ] || fail "missing $archive (run tools/pack.sh first, or download the published asset)"
sha="$(dirname "$archive")/$asset.sha256"
[ -f "$sha" ] || fail "missing $sha"
notes="docs/releases/$release.md"
body=$( [ -f "$notes" ] && cat "$notes" || echo "release $release" )

api="https://gitee.com/api/v5/repos/$slug"

echo "== create release $release on Gitee"
if [ "$dry_run" -eq 1 ]; then
    echo "   [dry-run] POST $api/releases  (tag_name=$release)"
    echo "   [dry-run] POST $api/releases/<id>/attach_files  ($asset, $sha)"
    exit 0
fi

payload=$(python3 - "$release" "$body" "$token" <<'PY'
import json, sys
tag, body, token = sys.argv[1], sys.argv[2], sys.argv[3]
print(json.dumps({"access_token": token, "tag_name": tag, "name": tag,
                  "body": body, "target_commitish": "master", "prerelease": False}))
PY
)
created=$(curl -fsS -X POST "$api/releases" -H 'Content-Type: application/json' -d "$payload") \
    || fail "release creation failed (token permissions? existing release?)"
id=$(printf '%s' "$created" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')
[ -n "$id" ] || fail "no release id in the response: $created"

for file in "$archive" "$sha"; do
    echo "== attach $(basename "$file")"
    curl -fsS -X POST "$api/releases/$id/attach_files" \
        -F "access_token=$token" -F "file=@$file" >/dev/null \
        || fail "attaching $(basename "$file") failed"
done

cat <<EOF

== done
   release: https://gitee.com/$slug/releases
   verify : curl -fL -o /tmp/$asset https://gitee.com/$slug/releases/download/$release/$asset
            curl -fL -o /tmp/$asset.sha256 https://gitee.com/$slug/releases/download/$release/$asset.sha256
            ( cd /tmp && sha256sum -c $asset.sha256 )
EOF
