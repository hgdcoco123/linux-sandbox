"""Console entry points that hand over to the bundled binary.

`pip install linux-sandbox` should give a working `sandbox`, not a Python reimplementation. The
binaries live in the linux_sandbox_bin package, next to each other, because the CLI locates its
launcher through /proc/self/exe.
"""
import os
import sys
from importlib.resources import files


def _run(name):
    exe = files("linux_sandbox_bin").joinpath(name)
    path = str(exe)
    if not os.access(path, os.X_OK):
        os.chmod(path, 0o755)
    os.execv(path, [path, *sys.argv[1:]])


def sandbox():
    """Same behaviour as the `sandbox` binary."""
    _run("sandbox")


def sbx():
    """Alias for environments that already have a `sandbox` command."""
    _run("sandbox")
