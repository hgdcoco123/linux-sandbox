#!/usr/bin/env bash
# The AI layer's own check: contracts plus the offline demo, on any host with Python 3.
# It never calls a model and never starts the sandbox, so a green run says the plumbing
# works - not that the diagnosis is good. docs/AI_VALIDATION.md lists what needs a human.
set -u
cd "$(dirname "$0")/.." || exit 1
python=${PYTHON:-python3}
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT

echo "== contract tests"
"$python" -m unittest discover -s tests/unit/ai || exit 1

echo "== offline demo"
"$python" -m sandbox_ai --db "$work/demo.sqlite3" --scope demo demo --output "$work/out" > "$work/demo.json" || exit 1
"$python" - "$work" <<'PY' || exit 1
import json, sys
work = sys.argv[1]
demo = json.load(open(work + '/demo.json'))
assert demo['statuses'] == ['completed'] * 3, demo['statuses']
assert demo['search'], 'lexical retrieval returned nothing'
analysis = json.load(open(work + '/out/analysis-1.json'))
assert analysis['status'] == 'completed'
assert analysis['metrics']['citations'] >= 1, analysis['metrics']
assert analysis['metrics']['context_chars'] > 0
assert analysis['evidence'], 'no evidence recorded'
print('== ok: demo completed with', analysis['metrics']['citations'], 'citations')
PY

echo "== aggregate metrics"
"$python" -m sandbox_ai --db "$work/demo.sqlite3" --scope demo stats > "$work/stats.json" || exit 1
"$python" - "$work" <<'PY' || exit 1
import json, sys
stats = json.load(open(sys.argv[1] + '/stats.json'))
assert stats['analyses'] == 3, stats
assert stats['structured_output_rate'] == 1.0, stats
assert stats['tool_denied'] == 0, stats
print('== ok: stats', json.dumps({k: stats[k] for k in
      ('analyses', 'completed', 'structured_output_rate', 'tool_calls', 'tool_denied')}))
PY

echo "== packaging metadata"
"$python" - <<'PY' || exit 1
import importlib
import tomllib
meta = tomllib.load(open('pyproject.toml', 'rb'))['project']
assert meta['version'], 'pyproject has no version'
assert 'LICENSE' in __import__('os').listdir('.'), 'LICENSE is missing (nobody may use the code)'
module, _, attribute = meta['scripts']['sandbox-ai'].partition(':')
entry = getattr(importlib.import_module(module), attribute)
assert callable(entry), 'the sandbox-ai entry point is not callable'
print('== ok: packaging', meta['name'], meta['version'], 'entry', meta['scripts']['sandbox-ai'])
PY

echo "AI CHECK PASSED: contracts, demo, aggregate metrics and packaging"
