"""End-to-end proof of the controlled verification loop, inside a delegated unit.

Run through tools/ai-verify-e2e.sh, which relaxes the host policy and starts this inside
`systemd-run --user -p Delegate=yes`. What it proves:

1. a real isolated run produces the analysis evidence (not a synthetic bundle),
2. the analysis proposes a registered template and the operator path executes it for real,
3. the new run is imported, the receipt is recorded, and the receipt still says
   "executed, not automatically confirmed",
4. the next turn of that conversation sees verification_context and can cite the
   verification run's evidence with origin=verification.

--skip-run validates the harness (configs, template registry, imports) without touching
the sandbox, so a dry run works on a host that cannot do isolation.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# Executed as `python3 tools/ai_verify_e2e.py` inside a transient unit: the repository root
# is not on sys.path, and the sandbox may be imported from there only.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sandbox_ai.evidence import collect
from sandbox_ai.models import canonical
from sandbox_ai.service import Analyzer
from sandbox_ai.store import Store
from sandbox_ai.verification import Templates, execute
from sandbox_ai.cli import stats

LIMITS = dict(wall_ms=5000, startup_ms=5000, grace_ms=500, memory_bytes=268435456,
              pids=32, capture_bytes=65536, tmp_bytes=16777216)


class Scripted:
    """Deterministic model stand-in: real dispatch, real validation, no diagnosis claim."""
    identity = 'e2e-scripted-v1'
    usage = []

    def __init__(self, replies):
        self.replies = list(replies)
        self.prompts = []

    def complete(self, messages, tools):
        self.prompts.append(json.loads(messages[1]['content']))
        if not self.replies:
            raise AssertionError('unexpected model call')
        value = self.replies.pop(0)
        return value(messages) if callable(value) else value


def analysis_reply(messages, predicate=None):
    evidence = json.loads(messages[1]['content'])['evidence']
    chosen = next((item for item in evidence if predicate is None or predicate(item)), evidence[0])
    return dict(role='assistant', content=canonical(dict(
        summary='脚本化模型：仅用于验证链路，不作诊断结论。',
        facts=[dict(statement='执行报告记录了本次结果。',
                    citations=[dict(id=chosen['id'], quote=chosen['text'][:60])])],
        hypotheses=[], missing_info=[], next_steps=[])))


def proposal_reply(template_id, reason):
    return dict(role='assistant', content=None, tool_calls=[dict(
        id='call-1', type='function', function=dict(name='propose_verification',
        arguments=canonical(dict(template_id=template_id, reason=reason,
                                 expected_observation='模板是否能在沙箱里写入 /output'))))])


def current_run_config(runtime, runs, fixture):
    return dict(schema_version=1, mode='isolated', network='none',
                runtime_root=str(runtime), cgroup_root=str(runs),
                argv=[fixture, 'session-read'],  # reads a file the workspace does not have
                limits=dict(wall_ms=1500, startup_ms=5000, grace_ms=200))


def template_config(runtime, runs, fixture, output):
    return dict(schema_version=1, mode='isolated', network='none', runtime_root=str(runtime),
                cgroup_root=str(runs), argv=[fixture, 'session-write'],
                mounts=[{'source': str(output), 'target': '/output', 'readonly': False}],
                limits=dict(LIMITS))


def write_harness(work, runtime, runs, fixture):
    """Configs an operator would prepare by hand: fixed argv, mounts, bounded limits."""
    templates = work/'templates'
    templates.mkdir(parents=True, exist_ok=True)
    output = work/'verify-output'
    output.mkdir(mode=0o700, exist_ok=True)
    os.chmod(output, 0o700)
    current = work/'current.json'
    current.write_text(json.dumps(current_run_config(runtime, runs, fixture)), encoding='utf-8')
    (templates/'write-probe.json').write_text(
        json.dumps(template_config(runtime, runs, fixture, output)), encoding='utf-8')
    (templates/'templates.json').write_text(json.dumps({'write_probe': {
        'description': '使用预先准备的程序检查沙箱能否在 /output 中创建文件',
        'config': 'write-probe.json'}}), encoding='utf-8')
    return current, templates


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--fixture', default='/usr/bin/sandbox-fixture')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--scope', default='e2e')
    parser.add_argument('--skip-run', action='store_true')
    args = parser.parse_args(argv)

    work = Path(args.work or tempfile.mkdtemp(prefix='sbx-ai-verify-')).resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.skip_run:
        runs = work/'runs'
        runs.mkdir(exist_ok=True)
    else:
        from delegated_cgroup import prepare
        runs = prepare()
    current, templates_dir = write_harness(work, args.runtime.resolve(), runs, args.fixture)
    templates = Templates(templates_dir/'templates.json')
    if args.skip_run:
        print('dry run ok: configs, template registry and digest load')
        print('work directory: ' + str(work))
        return 0

    state = work/'state'
    # The sandbox refuses a state root that is not private: mkdir's mode is advisory and
    # only applies on creation, so set it explicitly as well.
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(state, 0o700)
    completed = subprocess.run([str(args.sandbox.resolve()), 'run', '--config', str(current),
                                '--state-root', str(state)],
                               capture_output=True, text=True, timeout=120)
    (work/'current.cli.stdout').write_text(completed.stdout)
    (work/'current.cli.stderr').write_text(completed.stderr)
    try:
        report = json.loads(completed.stdout)
        run_id = report['run_id']
    except (ValueError, KeyError, TypeError):
        reason = (completed.stderr or completed.stdout).strip().splitlines()
        print('FAIL: the analysed run produced no report: ' +
              (reason[-1] if reason else 'no output'), file=sys.stderr)
        print('FAIL: see ' + str(work), file=sys.stderr)
        return 1
    store = Store(work/'state.db')
    try:
        bundle = collect(args.scope, state/run_id, context='端到端验证演练：当前运行为真实隔离执行。')
        store.put_bundle(bundle)
        print('== analysed run: ' + run_id + ' summary=' + str(bundle.execution.get('summary')))

        model = Scripted([proposal_reply('write_probe', '确认沙箱能否在 /output 写入，以排除工作区只读'),
                          analysis_reply])
        first = Analyzer(store, model, templates=templates).analyze(
            args.scope, run_id, '这次运行为什么失败？需要什么验证？')
        proposals = first['proposals']
        if first['status'] != 'completed' or len(proposals) != 1:
            print('FAIL: the analysis did not produce exactly one proposal: ' + json.dumps(
                dict(status=first['status'], error=first.get('error'))), file=sys.stderr)
            return 1

        receipt = execute(store, args.scope, first['analysis_id'], proposals[0]['id'], templates,
                          args.sandbox.resolve(), work/'verify')
        if receipt['status'] != 'executed_not_automatically_confirmed':
            print('FAIL: unexpected receipt status ' + str(receipt['status']), file=sys.stderr)
            return 1
        execution = receipt['execution']
        print('== verification run: ' + receipt['run_id'] + ' cli_exit=' + str(receipt['cli_exit']) +
              ' exit_code=' + str(execution.get('exit_code')) +
              ' isolated=' + str(execution.get('isolated')))
        if execution.get('isolated') is not True:
            print('FAIL: the verification run was not isolated', file=sys.stderr)
            return 1
        if (execution.get('cleanup') or {}).get('errors'):
            print('FAIL: the verification run did not clean up: ' +
                  json.dumps(execution['cleanup']), file=sys.stderr)
            return 1
        if execution.get('exit_code') != 0:
            print('FAIL: the verification template failed on its own: ' +
                  json.dumps({k: execution.get(k) for k in ('summary', 'exit_code', 'verdict')}),
                  file=sys.stderr)
            return 1
        print('real isolated verification executed')
        print('verification ran isolated and cleaned up')

        verified = store.bundle(args.scope, receipt['run_id'])
        recorded = store.verifications(args.scope, [first['analysis_id']])
        if not recorded:
            print('FAIL: the receipt was not recorded for the conversation', file=sys.stderr)
            return 1

        model = Scripted([lambda messages: analysis_reply(
            messages, lambda item: item.get('origin') == 'verification')])
        second = Analyzer(store, model).analyze(
            args.scope, run_id, '那次验证支持还是反驳了假设？', session_id=first['session_id'])
        prompt = model.prompts[0]
        context = prompt.get('verification_context') or []
        origins = {item.get('origin') for item in prompt['evidence']}
        if not context or context[0]['run_id'] != receipt['run_id']:
            print('FAIL: the next turn did not see the verification context', file=sys.stderr)
            return 1
        if 'verification' not in origins:
            print('FAIL: the verification run is not in the evidence', file=sys.stderr)
            return 1
        print('verification receipt visible to the next turn')
        if second['status'] != 'completed' or second['metrics']['citations'] < 1:
            print('FAIL: citing verification evidence did not validate: ' + json.dumps(
                second['metrics']), file=sys.stderr)
            return 1
        print('verification evidence is citable')

        summary = stats(store, args.scope)
        (work/'stats.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2))
        if summary['completed'] < 2 or summary['tool_denied'] != 0:
            print('FAIL: unexpected aggregate metrics ' + json.dumps(summary), file=sys.stderr)
            return 1
        print('== stats: ' + json.dumps({k: summary[k] for k in
              ('analyses', 'completed', 'tool_denied', 'verifications')}))
        print('== work directory kept at ' + str(work))
        return 0
    finally:
        store.close()


if __name__ == '__main__':
    raise SystemExit(main())
