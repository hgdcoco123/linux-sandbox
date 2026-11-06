"""Prepare a delegated cgroup subtree; the one place that knows this layout.

Extracted from delegated_run.py so the AI end-to-end driver prepares the same subtree the
acceptance harness does, instead of keeping a second copy that can drift.
"""
import os
from pathlib import Path


def prepare():
    """Turn the current delegated unit into supervisor/runs and return the runs path."""
    entry = next((line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines()
                  if line.startswith("0::")), None)
    if not entry:
        raise RuntimeError("unified cgroup v2 required")
    parent = Path("/sys/fs/cgroup") / entry.lstrip("/")
    if parent == Path("/sys/fs/cgroup"):
        raise RuntimeError("refusing to change global cgroup; start under systemd-run -p Delegate=yes")
    controllers = set((parent / "cgroup.controllers").read_text().split())
    if not {"cpu", "memory", "pids"} <= controllers:
        raise RuntimeError("delegated scope does not expose cpu/memory/pids controllers")
    processes = (parent / "cgroup.procs").read_text().split()
    if processes != [str(os.getpid())]:
        raise RuntimeError("delegated service must initially contain only this wrapper")
    control = parent / "supervisor"
    runs = parent / "runs"
    control.mkdir()
    (control / "cgroup.procs").write_text(str(os.getpid()))
    enabled = ["cpu", "memory", "pids"]
    if "io" in controllers:
        enabled.append("io")  # Optional observation only; no I/O hard limit is configured.
    flags = " ".join("+" + name for name in enabled)
    (parent / "cgroup.subtree_control").write_text(flags)
    runs.mkdir()
    (runs / "cgroup.subtree_control").write_text(flags)
    return runs
