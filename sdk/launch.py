"""Shared launch planning for CLI wrappers and SDKs; no privilege escalation.

Auto mode uses a caller-supplied cgroup delegation, otherwise systemd user
delegation. Process diagnostics always need explicit allow_unsafe.
"""
from dataclasses import dataclass
from pathlib import Path
import json
import os
import stat
import subprocess
import uuid

PREFIX = Path('/opt/linux-sandbox')


def protected(path):
    current = Path(path)
    while True:
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError('installation path is not root-owned and protected: ' + str(current))
        if current == current.parent:
            return
        current = current.parent


def delegation_command(binary, wrapper, *, config=None, test_runtime=None,
                       state_root=None, session=False, unit=None, batch=False):
    argv = ['/usr/bin/systemd-run', '--user', '--wait', '--pipe', '--collect',
            '-p', 'Delegate=yes', '-p', 'Type=exec']
    if unit:
        argv += ['--unit', unit]
    argv += ['/usr/bin/python3', str(wrapper), '--sandbox', str(binary)]
    if config:
        argv += ['--config', str(Path(config).resolve())]
    elif test_runtime:
        argv += ['--test-runtime', str(Path(test_runtime).resolve())]
    else:
        raise ValueError('config or test_runtime required')
    if state_root:
        argv += ['--state-root', str(Path(state_root).absolute())]
    if session:
        argv += ['--session']
    if batch:
        argv += ['--batch']
    return argv


@dataclass
class LaunchPlan:
    argv: list
    unit: str = None

    def stop(self, process):
        # Killing systemd-run alone does not stop the service it submitted.
        if self.unit:
            stopped = subprocess.run(['/usr/bin/systemctl', '--user', 'stop', self.unit],
                                     capture_output=True, timeout=15)
            if stopped.returncode:
                raise RuntimeError('could not stop startup unit ' + self.unit + ': ' +
                                   stopped.stderr.decode(errors='replace'))
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def plan_launch(binary, config_file, state_root, *, session=False,
                allow_unsafe=False, startup='auto', batch=False):
    if session and batch:
        raise ValueError('session and batch are mutually exclusive')
    if startup not in ('auto', 'direct', 'delegated'):
        raise ValueError('startup must be auto, direct or delegated')
    binary = Path(binary).resolve()
    config_file = Path(config_file).resolve()
    config = json.loads(config_file.read_text(encoding='utf-8'))
    if batch:
        tasks = config.get('tasks', [])
        if not tasks:
            raise ValueError('batch requires tasks')
        configs = [task['config'] for task in tasks]
        modes = {item.get('mode', 'isolated') for item in configs}
        if len(modes) != 1:
            raise ValueError('batch tasks must share execution mode')
        mode = next(iter(modes))
        delegated_root = all(item.get('cgroup_root') for item in configs)
    else:
        mode = config.get('mode', 'isolated')
        delegated_root = bool(config.get('cgroup_root'))
    if mode not in ('process', 'isolated'):
        raise ValueError('unknown execution mode')
    if mode == 'process' and not allow_unsafe:
        raise ValueError('process diagnostics require allow_unsafe=True')
    if mode == 'process' and startup == 'delegated':
        raise ValueError('delegated startup is for isolated execution')
    delegated = mode == 'isolated' and (startup == 'delegated' or
                  (startup == 'auto' and not delegated_root))
    if not delegated:
        argv = [str(binary), 'batch' if batch else ('session-open' if session else 'run'), '--config', str(config_file),
                '--state-root', str(state_root)]
        if allow_unsafe:
            argv.append('--allow-unsafe')
        return LaunchPlan(argv)
    if os.geteuid() == 0:
        raise ValueError('delegated startup requires an ordinary user')
    if binary == PREFIX/'bin/sandbox':
        wrapper = PREFIX/'share/linux-sandbox/tools/delegated_run.py'
        for path in (binary, binary.parent/'sandbox-launcher', wrapper,
                     PREFIX/'share/linux-sandbox/sdk/launch.py'):
            protected(path)
    else:
        wrapper = binary.parent.parent/'tools/delegated_run.py'
        if not wrapper.is_file():
            raise ValueError('delegation wrapper unavailable; use an installed binary or explicit cgroup_root')
    unit = 'linux-sandbox-' + uuid.uuid4().hex + '.service'
    return LaunchPlan(delegation_command(binary, wrapper, config=config_file,
                      state_root=state_root, session=session, unit=unit, batch=batch), unit)
