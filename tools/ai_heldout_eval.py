"""Held-out evaluation: fresh real runs that took no part in tuning, plus a two-turn loop.

Two things the record was missing:

- Every number so far came from four runs that the prompt was tuned against. These three are
  new and use different shapes (plain failure, stderr diagnosis, 200 lines of stdout), so a
  pass rate here is at least not the training set. It is still one host and one fixture
  family, which the output says out loud.
- The "propose -> execute -> ask again" loop was only tested with a scripted model. Here the
  two analyses use the real provider; the receipt is attached by this script to a verification
  run the drill already produced, because the drill's linked-list proposal belongs to its own
  scripted analysis.

Process mode is used on purpose: it needs no privilege, and the analysis layer does not care
whether a report says isolated=true.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sandbox_ai.cli import stats
from sandbox_ai.evidence import collect, line_reference
from sandbox_ai.models import canonical
from sandbox_ai.providers import CompatibleProvider
from sandbox_ai.service import Analyzer
from sandbox_ai.store import Store
from sandbox_ai.verification import Templates

QUESTION = '这次运行为什么失败？请区分已确认事实、原因假设和仍缺少的信息。'

CASES = [
    ('plain-failure', ['/bin/sh', '-c', 'exit 7'],
     '进程以退出码 7 结束，没有任何输出。'),
    ('stderr-diagnosis', ['/bin/sh', '-c', 'echo "error: cannot open libfoo.so.2" >&2; exit 1'],
     '缺少共享库导致的启动失败。'),
    ('many-lines', ['/bin/sh', '-c', 'i=0; while [ $i -lt 200 ]; do echo "line $i"; i=$((i+1)); done'],
     '输出 200 行后正常退出。'),
]


def run_case(binary, work, name, argv):
    folder = work/'runs'/name
    folder.mkdir(parents=True, exist_ok=True)
    config = folder/'config.json'
    config.write_text(json.dumps(dict(
        schema_version=1, mode='process', network='host', cwd='/tmp', argv=argv,
        limits=dict(wall_ms=5000, startup_ms=5000, grace_ms=200))))
    state = folder/'state'
    state.mkdir(mode=0o700, exist_ok=True)
    os.chmod(state, 0o700)
    completed = subprocess.run([str(binary), 'run', '--config', str(config),
                                '--allow-unsafe', '--state-root', str(state)],
                               capture_output=True, text=True, timeout=120)
    (folder/'cli.stdout').write_text(completed.stdout)
    (folder/'cli.stderr').write_text(completed.stderr)
    try:
        report = json.loads(completed.stdout)
    except ValueError:
        raise SystemExit('FAIL: ' + name + ' produced no report: ' +
                         (completed.stderr or '').strip()[:200])
    return state/report['run_id']


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--drill', type=Path,
                        help='A drill work directory: its analysed run, verification run and templates')
    parser.add_argument('--model', default=os.environ.get('SANDBOX_AI_MODEL', ''))
    parser.add_argument('--base-url', default=os.environ.get('SANDBOX_AI_BASE_URL', ''))
    parser.add_argument('--key-env', default='SANDBOX_AI_API_KEY')
    parser.add_argument('--rerank', action='store_true')
    parser.add_argument('--two-turn-only', action='store_true', help='Skip the held-out cases')
    args = parser.parse_args(argv)

    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    store = Store(work/'heldout.sqlite3')
    try:
        provider = CompatibleProvider(args.base_url, args.model, key_env=args.key_env,
                                      rerank_model=os.environ.get('SANDBOX_AI_RERANK_MODEL', '')
                                      if args.rerank else '',
                                      rerank_path=os.environ.get('SANDBOX_AI_RERANK_PATH', '/rerank'))
        rows = []
        for name, command, description in ([] if args.two_turn_only else CASES):
            run_dir = run_case(args.sandbox.resolve(), work, name, command)
            bundle = collect('heldout', run_dir, context=description)
            store.put_bundle(bundle)
            result = Analyzer(store, provider, semantic=False,
                              reranker=provider if args.rerank else None).analyze(
                'heldout', bundle.run_id, QUESTION)
            rows.append(dict(case=name, execution=result['execution'].get('summary'),
                             status=result['status'], citations=result['metrics']['citations'],
                             repairs=result['metrics']['repairs'],
                             seconds=result['elapsed_seconds'],
                             error=result.get('error'),
                             missing=len((result['analysis'] or {}).get('missing_info') or [])))
            print('== ' + name + ': ' + result['status'] + ', ' +
                  str(result['metrics']['citations']) + ' citations, ' +
                  str(result['elapsed_seconds']) + 's', file=sys.stderr)
        summary = stats(store, 'heldout')
        print('== held-out aggregate: ' + json.dumps(summary, ensure_ascii=False), file=sys.stderr)

        two_turn = None
        if args.drill:
            two_turn = two_turn_loop(store, provider, args, work)
        print(json.dumps(dict(cases=rows, aggregate=summary, two_turn=two_turn),
                         ensure_ascii=False, indent=2))
        failed = [row for row in rows if row['status'] != 'completed']
        if rows and len(failed) == len(rows):
            print('FAIL: every held-out case failed validation', file=sys.stderr)
            return 1
        print('held-out: ' + str(len(rows) - len(failed)) + '/' + str(len(rows)) +
              ' passed validation', file=sys.stderr)
        return 0
    finally:
        store.close()


def two_turn_loop(store, provider, args, work):
    """Real model proposes, a real verification run is attached, then it is asked again."""
    drill = args.drill.resolve()
    analysed = sorted((drill/'state').glob('run-*'))
    verified = sorted((drill/'verify'/'s').glob('run-*'))
    if not analysed or not verified:
        raise SystemExit('FAIL: drill directory has no analysed/verification run')
    bundle = collect('twoturn', analysed[0], context='演练产生的真实隔离运行。')
    store.put_bundle(bundle)
    templates = Templates(drill/'templates'/'templates.json')
    first = Analyzer(store, provider, templates=templates, require_proposal=True).analyze(
        'twoturn', bundle.run_id,
        '这次运行为什么失败？如果可以用登记的检查确认，请先调用 propose_verification '
        '选择一个模板，再给出分析。')
    if not first['proposals']:
        return dict(status='no_proposal', analysis=first['status'])
    # The verification run is real (produced by the drill); its receipt belongs to the drill's
    # scripted analysis, so this script writes a receipt that points the *real* analysis at it.
    receipt = json.loads((drill/'verify'/'verification.json').read_text())
    store.save_verification('twoturn', dict(
        receipt, analysis_id=first['analysis_id'], proposal_id=first['proposals'][0]['id'],
        scope='twoturn', hypothesis='这次失败由目标自身退出码造成'))
    verified_bundle = collect('twoturn', verified[0], context='操作者执行登记模板产生的验证运行。')
    store.put_bundle(verified_bundle)
    second = Analyzer(store, provider).analyze(
        'twoturn', bundle.run_id, '那次验证支持还是反驳了假设？', session_id=first['session_id'])
    updates = (second['analysis'] or {}).get('hypothesis_updates') or []
    return dict(status=second['status'], updates=updates,
                verification_run=verified_bundle.run_id,
                citations=second['metrics']['citations'],
                prompt_has_context=bool(second['metrics']))


if __name__ == '__main__':
    raise SystemExit(main())
