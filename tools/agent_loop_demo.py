"""Does the analysis actually help the next attempt?

The goal deliberately does NOT say what the answer should be: the requirement lives in
`spec.txt`, which the faulty program never reads and the agent never sees directly. So:

- raw stderr says nothing about the requirement (the failing programs are silent),
- the structured analysis cites `spec.txt` and the artifact, i.e. it carries the missing fact.

That is the information asymmetry a real agent faces: the task is "make the check pass", not
"write 42". If the explanation is worth anything, it must move the success rate.

Three feedback conditions - none / stderr (+ exit code) / analysis - and two agents: a rule
table (proves the output is machine-consumable) and a real model (proves it helps).

Process mode with cwd pointing at the attempt directory: no privilege, no mounts, and the
verifier reads the artifact from disk.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sandbox_ai.evidence import collect
from sandbox_ai.providers import CompatibleProvider, DemoProvider
from sandbox_ai.service import Analyzer
from sandbox_ai.store import Store

QUESTION = '这次运行为什么没有达到目标？下一步应该改什么？'


def task_write_from_spec():
    return dict(
        name='write-from-spec',
        spec='expected=42\n',
        goal='让 answer.txt 满足 spec.txt 的要求，并且这次运行判定为成功',
        faulty='echo 41 > answer.txt',
        expect=lambda text, code: text.strip() == '42' and code == 0)


def task_exit_clean_from_spec():
    return dict(
        name='exit-clean-from-spec',
        spec='expected=done\n',
        goal='让 answer.txt 满足 spec.txt 的要求，并且这次运行判定为成功',
        faulty='echo done > answer.txt; exit 1',
        expect=lambda text, code: text.strip() == 'done' and code == 0)


def task_trim_to_limit():
    return dict(
        name='trim-to-limit',
        spec='expected=ok\nmax_bytes=64\n',
        goal='让 answer.txt 满足 spec.txt 的要求，并且这次运行判定为成功',
        faulty='yes x | head -c 4096 > answer.txt',
        expect=lambda text, code: text.strip() == 'ok' and 0 < len(text) <= 64 and code == 0)


TASKS = (task_write_from_spec, task_exit_clean_from_spec, task_trim_to_limit)


def run_program(binary, work, program, spec_dir):
    attempt = work/('attempt-%d' % (len(list(work.glob('attempt-*'))) + 1))
    attempt.mkdir(parents=True)
    config = attempt/'config.json'
    config.write_text(json.dumps(dict(
        schema_version=1, mode='process', network='host', cwd=str(attempt),
        argv=['/bin/sh', '-c', program],
        limits=dict(wall_ms=5000, startup_ms=5000, grace_ms=200))))
    state = attempt/'state'
    state.mkdir(mode=0o700)
    os.chmod(state, 0o700)
    done = subprocess.run([str(binary), 'run', '--config', str(config),
                           '--allow-unsafe', '--state-root', str(state)],
                          capture_output=True, text=True, timeout=120)
    (attempt/'cli.stderr').write_text(done.stderr)
    try:
        report = json.loads(done.stdout)
    except ValueError:
        raise SystemExit('attempt produced no report: ' + done.stderr.strip()[:200])
    return dict(attempt=attempt, report=report, run_dir=state/report['run_id'])


def write_spec(work, task):
    # One directory per task. A shared directory leaks the other tasks' expected values into
    # this task's evidence, and the analysis then quotes the wrong requirement.
    specs = work/'specs'/task['name']
    specs.mkdir(parents=True, exist_ok=True)
    (specs/'spec.txt').write_text(task['spec'])
    return specs


def verify(task, attempt):
    artifact = attempt['attempt']/'answer.txt'
    text = artifact.read_text(errors='replace') if artifact.exists() else ''
    code = attempt['report'].get('exit_code')
    return bool(task['expect'](text, code)), text, code


def hint_text(stderr_text, code, analysis):
    """What the agent is allowed to know, per condition."""
    parts = [stderr_text or '', 'process exit code: ' + str(code)]
    if analysis:
        body = analysis.get('analysis') or {}
        parts.append(body.get('summary') or '')
        parts.extend(item.get('statement', '') for item in body.get('facts') or [])
        parts.extend((item.get('action') if isinstance(item, dict) else str(item)) or ''
                     for item in body.get('next_steps') or [])
        parts.extend(str(item) for item in body.get('missing_info') or [])
        for item in analysis.get('resolved_citations') or []:
            parts.append(item.get('text') or '')
    return ' | '.join(part for part in parts if part)


def rule_agent(condition, hint):
    """Act only on what the condition provided; refuse to guess otherwise."""
    if condition == 'none':
        return None
    limit = re.search(r'max_bytes=(\d+)', hint)
    expected = re.search(r'expected=(\w+)', hint)
    if expected and limit:
        return 'printf %s "' + expected.group(1) + '" > answer.txt'
    if expected:
        return 'echo ' + expected.group(1) + ' > answer.txt'
    return None                                   # no requirement in the feedback: no fix


MODEL_PROMPT = '''You are fixing a program that did not meet its goal. Reply with ONE shell
command only, no prose, no markdown fence.

Goal: {goal}

What you know about the last run:
{feedback}
'''


def model_agent(provider, task, hint):
    message = provider.complete([dict(role='user', content=MODEL_PROMPT.format(
        goal=task['goal'], feedback=hint or '(nothing)'))], [])
    content = (message.get('content') or '').strip()
    fenced = re.search(r'```(?:sh|bash)?\s*(.+?)```', content, re.S)
    lines = (fenced.group(1) if fenced else content).strip().splitlines()
    return lines[0] if lines else None


def run_loop(args, task, condition, agent, store, provider):
    work = args.work/task['name']/condition/agent
    work.mkdir(parents=True, exist_ok=True)
    program = task['faulty']
    history = []
    for number in range(1, args.attempts + 1):
        spec_dir = write_spec(args.work, task)
        attempt = run_program(args.sandbox.resolve(), work, program, spec_dir)
        passed, text, code = verify(task, attempt)
        history.append(dict(attempt=number, program=program, exit_code=code,
                            artifact=text[:60], ok=passed))
        if passed:
            return dict(task=task['name'], condition=condition, agent=agent,
                        success=True, attempts=number, history=history)
        stderr_file = attempt['run_dir']/'stderr.bin'
        stderr_text = stderr_file.read_text(errors='replace') if stderr_file.exists() else ''
        analysis = None
        if condition == 'analysis':
            # The requirement is only visible here, never to the program: that is the
            # information asymmetry the analysis is supposed to close.
            # One scope per task: a shared scope lets one task's evidence act as 'history'
            # for another, and the analysis then quotes the wrong requirement.
            scope = 'loop-' + task['name']
            bundle = collect(scope, attempt['run_dir'], artifact_root=spec_dir,
                             context=task['goal'])
            store.put_bundle(bundle)
            analysis = Analyzer(store, provider).analyze(scope, bundle.run_id, QUESTION)
            history[-1]['analysis_status'] = analysis['status']
        if condition == 'report':
            # Fair baseline: the whole report, not a subset of it - anyone with the sandbox
            # has this, so the analysis has to beat *this*, not just the stderr text.
            hint = json.dumps(attempt['report'], ensure_ascii=False)[:2000]
        else:
            hint = hint_text(stderr_text, code, analysis) if condition != 'none' else ''
        if agent == 'rule':
            program = rule_agent(condition, hint) or program
        else:
            program = model_agent(provider, task, hint) or program
        history[-1]['hint'] = hint[:300]
    return dict(task=task['name'], condition=condition, agent=agent, success=False,
                attempts=args.attempts, history=history)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--provider', choices=['demo', 'compatible'], default='compatible')
    parser.add_argument('--model', default=os.environ.get('SANDBOX_AI_MODEL', ''))
    parser.add_argument('--base-url', default=os.environ.get('SANDBOX_AI_BASE_URL', ''))
    parser.add_argument('--key-env', default='SANDBOX_AI_API_KEY')
    parser.add_argument('--attempts', type=int, default=3)
    parser.add_argument('--agents', default='rule,model')
    parser.add_argument('--conditions', default='none,report,analysis')
    parser.add_argument('--tasks', default='')
    args = parser.parse_args(argv)

    args.work.mkdir(parents=True, exist_ok=True)
    provider = (DemoProvider() if args.provider == 'demo'
                else CompatibleProvider(args.base_url, args.model, key_env=args.key_env))
    store = Store(args.work/'agent_loop.sqlite3')
    try:
        chosen = [factory for factory in TASKS
                  if not args.tasks or factory()['name'] in args.tasks.split(',')]
        results = []
        for factory in chosen:
            task = factory()
            for condition in [item for item in args.conditions.split(',') if item]:
                for agent in [item for item in args.agents.split(',') if item]:
                    outcome = run_loop(args, task, condition, agent, store, provider)
                    results.append(outcome)
                    print('== ' + task['name'] + ' / ' + condition + ' / ' + agent + ': ' +
                          (('ok in ' + str(outcome['attempts']) + ' attempt(s)')
                           if outcome['success'] else 'not fixed'), file=sys.stderr)
        table = {}
        for outcome in results:
            row = table.setdefault(outcome['agent'] + '+' + outcome['condition'],
                                   dict(runs=0, fixed=0, attempts=0))
            row['runs'] += 1
            row['fixed'] += 1 if outcome['success'] else 0
            row['attempts'] += outcome['attempts']
        for row in table.values():
            row['avg_attempts'] = round(row['attempts'] / row['runs'], 2)
        print(json.dumps(dict(results=results, table=table), ensure_ascii=False, indent=2))
        print('== summary', file=sys.stderr)
        for key in sorted(table):
            row = table[key]
            print('   ' + key + ': fixed ' + str(row['fixed']) + '/' + str(row['runs']) +
                  ', avg attempts ' + str(row['avg_attempts']), file=sys.stderr)
        return 0
    finally:
        store.close()


if __name__ == '__main__':
    raise SystemExit(main())
