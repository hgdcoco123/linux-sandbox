#!/usr/bin/env python3
"""Run one command in the sandbox from CI and print its report.

This is the step a CI job needs: it turns the delegated transient unit it runs in into a
usable cgroup subtree (through tools/delegated_cgroup.py, the same helper the acceptance
harness uses), then runs the command in isolated mode and reports what happened.

It never falls back to `process` mode silently: without a delegated cgroup it fails and says
so, unless the caller explicitly allowed the diagnostic backend.

Exit codes: 0 when the report's verdict is `ok`, 3 otherwise (the report is still printed and
written, because a failing run is the interesting one), 2 on a usage or setup error.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from delegated_cgroup import prepare as prepare_delegated_cgroup  # noqa: E402


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', required=True)
    parser.add_argument('--command', required=True,
                        help='the target command line, split like a shell would')
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--output', default='sandbox-output')
    parser.add_argument('--input', default='')
    parser.add_argument('--timeout-ms', type=int, default=5000)
    parser.add_argument('--memory', default='256M')
    parser.add_argument('--report', default='sandbox-report.json')
    parser.add_argument('--allow-process', action='store_true')
    args = parser.parse_args(argv)

    sandbox = Path(args.sandbox).resolve()
    if not os.access(sandbox, os.X_OK):
        print('sandbox binary is not executable: ' + str(sandbox), file=sys.stderr)
        return 2
    target = shlex.split(args.command)
    if not target:
        print('--command is empty', file=sys.stderr)
        return 2
    # systemd-run starts the command with the unit's working directory (the user's home), not
    # the caller's, so relative paths are resolved against the workspace the caller meant.
    base = Path(os.environ.get('GITHUB_WORKSPACE') or os.getcwd())
    output = (base / args.output).resolve() if not Path(args.output).is_absolute() else Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    report_path = (Path(args.report) if Path(args.report).is_absolute() else base / args.report)

    config = {'schema_version': 1, 'argv': target, 'network': 'none'}
    # Ask the environment before preparing anything: an isolated run that cannot unshare fails
    # deep inside with a host error, which is useless in a CI log.
    diagnostic = subprocess.run([str(sandbox), 'doctor'], capture_output=True, text=True,
                               timeout=10)
    probe = {}
    if diagnostic.returncode == 0 and diagnostic.stdout.strip():
        probe = (json.loads(diagnostic.stdout).get('isolation_probe') or {})
    runs = None
    why = ''
    try:
        runs = prepare_delegated_cgroup()
    except (RuntimeError, OSError) as error:
        runs = None
        why = str(error)  # the precise reason, instead of a bare "not delegated"
    # A delegated cgroup is necessary but not sufficient: unprivileged namespaces can be
    # restricted independently, and the run then fails deep inside with a host error.
    blocked = bool(probe) and not probe.get('available', False)
    if runs is not None and not blocked:
        config.update(mode='isolated', runtime_root=str(Path(args.runtime).resolve()),
                      cgroup_root=str(runs))
    elif args.allow_process:
        print('::warning::running the diagnostic backend, which is not isolated '
              '(report says isolated=false)', file=sys.stderr)
        # The diagnostic backend rejects mounts, so the target runs with cwd at the output
        # directory: relative writes still land where the action says artifacts go.
        config.update(mode='process', network='host', cwd=str(output))
    else:
        reason = ''
        if blocked:
            reason = (' (environment: isolation unavailable at stage ' + str(probe.get('stage')) +
                      ': ' + str(probe.get('error', 'unknown')) + ')')
        elif runs is None:
            reason = ' (' + (why or 'this process was not delegated a cgroup') + ')'
        print('cannot run isolated' + reason + ': run this step inside '
              '`systemd-run --user -p Delegate=yes`, or pass --allow-process to accept the '
              'diagnostic backend', file=sys.stderr)
        return 2
    mounts = []
    if config.get('mode') == 'process':
        pass  # no mounts: the backend refuses them and cwd already points at the output
    elif args.input:
        mounts.append({'source': str(Path(args.input).resolve()), 'target': '/input',
                       'readonly': True})
    if config.get('mode') != 'process':
        mounts.append({'source': str(output), 'target': '/output', 'readonly': False})
    if mounts:
        config['mounts'] = mounts
    config['limits'] = {'wall_ms': args.timeout_ms}
    if args.memory:
        suffix = args.memory[-1].upper()
        scale = {'K': 1024, 'M': 1024 ** 2, 'G': 1024 ** 3}.get(suffix)
        number = args.memory[:-1] if scale else args.memory
        try:
            config['limits']['memory_bytes'] = int(number) * (scale or 1)
        except ValueError:
            print('--memory must be bytes or a number with K/M/G', file=sys.stderr)
            return 2

    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as handle:
        json.dump(config, handle, indent=2)
        config_path = handle.name
    try:
        argv = [str(sandbox), 'run', '--config', config_path]
        if config.get('mode') == 'process':
            argv.append('--allow-unsafe')  # the diagnostic backend always requires it
        done = subprocess.run(argv,
                              capture_output=True, text=True, timeout=args.timeout_ms / 1000 + 60)
    finally:
        os.unlink(config_path)
    if done.stdout.strip():
        report = json.loads(done.stdout)
    else:
        print('no report: ' + done.stderr.strip()[:400], file=sys.stderr)
        return 2
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    export = ((report.get('workspace') or {}).get('export') or {})
    print('verdict=' + str(report.get('verdict')))
    print('summary=' + str(report.get('summary')))
    print('run_id=' + str(report.get('run_id')))
    print('exit_code=' + str(report.get('exit_code')))
    print('artifacts_ok=' + str(export.get('ok', False)).lower())
    if export.get('manifest'):
        print('manifest=' + str(export['manifest']))
    return 0 if report.get('verdict') == 'ok' else 3


if __name__ == '__main__':
    raise SystemExit(main())
