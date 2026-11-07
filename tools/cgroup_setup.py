#!/usr/bin/env python3
"""Find or prepare a cgroup subtree the sandbox may use, without systemd.

The sandbox needs one thing from the environment: a cgroup v2 subtree it can write to, that is
empty of processes, with the cpu/memory/pids controllers available. Under systemd that is what
`systemd-run --user -p Delegate=yes` hands you. In a container there is no systemd, so this
script does the same job directly and prints the path to pass as `--cgroup-root`.

    cgroup_setup.py            # prepare and print the path (exit 0) or explain why not (exit 1)
    cgroup_setup.py --check    # only report what would be used

It never changes anything outside the cgroup this process already lives in, and never touches
/sys/fs/cgroup itself.
"""
import argparse
import os
from pathlib import Path
import sys


def our_cgroup():
    for line in Path('/proc/self/cgroup').read_text().splitlines():
        if line.startswith('0::'):
            return Path('/sys/fs/cgroup') / line[3:].lstrip('/')
    return None


def enable(where, controllers):
    """Enable controllers in one cgroup; returns the ones that are now available."""
    for controller in controllers:
        try:
            with open(where/'cgroup.subtree_control', 'w') as handle:
                handle.write('+' + controller)
        except PermissionError:
            raise
        except OSError:
            pass  # already enabled, or the controller is busy further up
    try:
        return set((where/'cgroup.controllers').read_text().split())
    except OSError:
        return set()


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='report, do not create anything')
    parser.add_argument('--name', default='sandbox-runs')
    args = parser.parse_args(argv)

    parent = our_cgroup()
    if parent is None:
        print('this kernel does not expose a unified cgroup v2 hierarchy', file=sys.stderr)
        return 1
    if not (parent/'cgroup.controllers').exists():
        print('cgroup v2 is not mounted at ' + str(parent), file=sys.stderr)
        return 1

    runs = parent/args.name
    if args.check:
        print(str(runs) if runs.exists() else 'would create ' + str(runs))
        return 0

    try:
        runs.mkdir(mode=0o700, exist_ok=True)
    except PermissionError:
        print('cannot create ' + str(runs) + ': this process has no write access to its cgroup\n'
              '  - in a container: run with --cgroupns=private and enough privilege, or mount a\n'
              '    writable cgroup subtree and pass --cgroup-root\n'
              '  - under systemd: run inside `systemd-run --user -p Delegate=yes`', file=sys.stderr)
        return 1
    try:
        enable(parent, ('cpu', 'memory', 'pids'))
        available = enable(runs, ('cpu', 'memory', 'pids'))
    except PermissionError:
        print('the cgroup ' + str(parent) + ' is not delegated to this process\n'
              '  - under systemd: `systemd-run --user -p Delegate=yes`\n'
              '  - in a container: --privileged, or a subtree chowned to this user', file=sys.stderr)
        return 1
    missing = {'cpu', 'memory', 'pids'} - available
    if missing:
        print('controllers unavailable in ' + str(runs) + ': ' + ', '.join(sorted(missing)) + '\n'
              '  the parent cgroup still has processes or does not delegate these controllers;\n'
              '  under systemd use `systemd-run --user -p Delegate=yes`, in a container use a\n'
              '  private cgroup namespace so this cgroup is the hierarchy root', file=sys.stderr)
        return 1
    print(str(runs))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
