"""An OJ worker integration test; executes only bundled fixtures, never user input."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys


def judge(report, stdout, expected, isolated=True):
    """Judge outside the sandbox. Infrastructure errors take precedence."""
    cleanup = report.get('cleanup', {})
    streams = [report.get(name, {}) for name in ('stdout', 'stderr')]
    if (isolated and report.get('isolated') is not True
            or report.get('errors') or cleanup.get('errors')
            or report.get('verdict') == 'cleanup_incomplete'
            or cleanup.get('descendants_empty') is not True
            or cleanup.get('launcher_reaped') is not True
            or (isolated and cleanup.get('cgroup_removed') is not True)
            or any(s.get('eof') is not True or s.get('flushed') is not True
                   or s.get('write_error') or s.get('failed_bytes', 0) for s in streams)):
        return 'SYSTEM_ERROR'
    summary = report.get('summary')
    if summary in ('timeout', 'oom', 'nonzero_exit', 'signaled'):
        return {'timeout': 'TIME_LIMIT', 'oom': 'MEMORY_LIMIT',
                'nonzero_exit': 'RUNTIME_ERROR', 'signaled': 'RUNTIME_ERROR'}[summary]
    if summary != 'success' or report.get('exit_code') != 0:
        return 'SYSTEM_ERROR'
    if any(s.get('discarded_bytes', 0) for s in streams):
        return 'OUTPUT_LIMIT'
    # This exercise deliberately uses whitespace-insensitive token comparison.
    return 'ACCEPTED' if stdout.split() == expected.split() else 'WRONG_ANSWER'


CASES = [
    ('correct', 'a,b=map(int,input().split()); print(a+b)', b'2 3\n', b'5\n', 'ACCEPTED'),
    ('wrong_answer', 'print(6)', b'2 3\n', b'5\n', 'WRONG_ANSWER'),
    ('runtime_error', 'raise RuntimeError("fixture")', b'', b'', 'RUNTIME_ERROR'),
    ('timeout', 'while True: pass', b'', b'', 'TIME_LIMIT'),
    ('output_limit', 'import os; os.write(1,b"x"*65536)', b'', b'x'*4096, 'OUTPUT_LIMIT'),
    ('descendant_cleanup', 'import os,time,signal\nif os.fork()==0:\n os.setsid()\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n time.sleep(30)\nelse:\n print(5)', b'', b'5', 'ACCEPTED'),
    ('after_failures', 'print(sum(map(int,input().split())))', b'-4 9\n', b'5', 'ACCEPTED'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True,
                        help='Known-working isolated profile with a Python runtime')
    parser.add_argument('--python', default='/usr/bin/python3', help='Interpreter inside runtime')
    parser.add_argument('--results', type=Path, required=True, help='New, short absolute evidence directory')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('Real integration requires Linux; use test_judge.py for portable checks')
    template = json.loads(args.config.read_text(encoding='utf-8'))
    if template.get('mode', 'isolated') != 'isolated':
        parser.error('Only isolated mode is accepted; there is no process fallback')
    # Resolve host paths using the same base as the original config file.
    for key in ('runtime_root', 'cgroup_root'):
        if template.get(key):
            template[key] = str((args.config.resolve().parent / template[key]).resolve())
    sys.path.insert(0, str(args.repo.resolve()))
    from sdk.launch import plan_launch

    root = args.results.resolve()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    results = []
    for name, code, stdin, expected, wanted in CASES:
        folder = root / name
        folder.mkdir(mode=0o700)
        source = folder / 'input'
        source.mkdir()
        (source / 'main.py').write_text(code + '\n', encoding='utf-8')
        (folder / 'stdin.txt').write_bytes(stdin)
        config = copy.deepcopy(template)
        config.update(mode='isolated', network='none', cwd='/work',
                      argv=[args.python, '-I', '-B', '/input/main.py'],
                      stdin_file=str(folder / 'stdin.txt'),
                      mounts=[{'source': str(source), 'target': '/input', 'readonly': True}])
        config.setdefault('limits', {}).update(wall_ms=1500, startup_ms=10000,
                                               grace_ms=100, capture_bytes=4096,
                                               memory_bytes=268435456, pids=32)
        config_path = folder / 'config.json'
        config_path.write_text(json.dumps(config), encoding='utf-8')
        state = folder / 's'
        state.mkdir(mode=0o700)
        plan = plan_launch(args.binary, config_path, state)
        process = subprocess.Popen(plan.argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            out, err = process.communicate(timeout=40)
        except BaseException:
            plan.stop(process)
            raise
        (folder / 'cli.stdout').write_bytes(out)
        (folder / 'cli.stderr').write_bytes(err)
        report = json.loads(out)  # Launch or malformed report errors fail the test.
        (folder / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        run_id = report['run_id']
        if Path(run_id).name != run_id or not run_id.startswith('run-'):
            raise AssertionError('Invalid run ID')
        stdout = (state / run_id / 'stdout.bin').read_bytes()
        actual = judge(report, stdout, expected)
        entry = dict(case=name, expected=wanted, actual=actual,
                     cli_exit=process.returncode, passed=actual == wanted)
        results.append(entry)
        (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        print(f'{name}: {actual} (expected {wanted})', flush=True)
        if actual == 'SYSTEM_ERROR':
            raise AssertionError(f'Infrastructure failure; inspect {folder}')
    if not all(item['passed'] for item in results):
        raise AssertionError(f'Integration failed; inspect {root / "results.json"}')
    print(f'PASS: {len(results)} real sandbox runs; evidence: {root}')


if __name__ == '__main__':
    main()
