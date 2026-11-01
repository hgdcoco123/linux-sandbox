"""Run inside an existing systemd Delegate=yes service; never changes the global cgroup root."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys

p = argparse.ArgumentParser()
p.add_argument("--sandbox", type=Path, required=True)
group = p.add_mutually_exclusive_group(required=True)
group.add_argument("--config", type=Path)
group.add_argument("--test-runtime", type=Path)
p.add_argument("--state-root", type=Path)
p.add_argument("--session", action="store_true", help="force persistent session startup")
p.add_argument("--batch", action="store_true")
a = p.parse_args()
if a.batch and (a.session or a.test_runtime):
    p.error("--batch requires --config and cannot be combined with --session")
if sys.platform != "linux":
    p.error("Linux only")
binary = str(a.sandbox.resolve())
needs_isolation = bool(a.test_runtime)
if a.config:
    needs_isolation = json.loads(a.config.read_text()).get("mode", "isolated") == "isolated"
if needs_isolation:
    # Check the real launcher before preparing any cgroup children or running jobs.
    diagnostic = subprocess.run([binary, "doctor"], capture_output=True, text=True, timeout=6)
    if diagnostic.returncode:
        p.error("environment diagnosis failed: " + diagnostic.stderr.strip())
    probe = json.loads(diagnostic.stdout).get("isolation_probe", {})
    if not probe.get("available", False):
        p.error("isolated execution is unavailable at stage " + str(probe.get("stage")) +
                ": " + str(probe.get("error", "unknown error")) +
                ". No policy was changed. See docs/DEPLOYMENT.md; full doctor output: " +
                json.dumps(probe, ensure_ascii=False))
entry = next((line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines() if line.startswith("0::")), None)
if not entry:
    p.error("unified cgroup v2 required")
parent = Path("/sys/fs/cgroup") / entry.lstrip("/")
if parent == Path("/sys/fs/cgroup"):
    p.error("refusing to change global cgroup; start under systemd-run -p Delegate=yes")
controllers = set((parent / "cgroup.controllers").read_text().split())
if not {"cpu", "memory", "pids"} <= controllers:
    p.error("delegated scope does not expose cpu/memory/pids controllers")
processes = (parent / "cgroup.procs").read_text().split()
if processes != [str(os.getpid())]:
    p.error("delegated service must initially contain only this wrapper")
control = parent / "supervisor"
runs = parent / "runs"
control.mkdir()
(control / "cgroup.procs").write_text(str(os.getpid()))
enabled = ["cpu", "memory", "pids"]
if "io" in controllers:
    enabled.append("io")  # Optional observation only; no I/O hard limit is configured.
control_flags = " ".join("+" + name for name in enabled)
(parent / "cgroup.subtree_control").write_text(control_flags)
runs.mkdir()
(runs / "cgroup.subtree_control").write_text(control_flags)
binary = str(a.sandbox.resolve())
if a.test_runtime:
    env = dict(os.environ, SANDBOX_TEST_RUNTIME=str(a.test_runtime.resolve()), SANDBOX_TEST_CGROUP=str(runs))
    for test in ("isolation.py", "sessions.py", "workspace_quota.py", "batch_integration.py"):
        code = subprocess.call([sys.executable, str(Path(__file__).resolve().parents[1] / "tests" / test), binary], env=env)
        if code:
            raise SystemExit(code)
    raise SystemExit(0)

config_path = a.config.resolve()
config = json.loads(config_path.read_text())
configs = [task["config"] for task in config["tasks"]] if a.batch else [config]
for item in configs:
    if item.get("mode", "isolated") != "isolated":
        p.error("delegated batch requires isolated tasks")
    for key in ("runtime_root", "stdin_file"):
        if item.get(key):
            item[key] = str((config_path.parent / item[key]).resolve())
    for m in item.get("mounts", []):
        m["source"] = str((config_path.parent / m["source"]).resolve())
    item["cgroup_root"] = str(runs)
with tempfile.TemporaryDirectory(prefix="sbxconf-") as temp:
    prepared = Path(temp) / "config.json"
    prepared.write_text(json.dumps(config))
    command = [binary, "batch" if a.batch else ("session-open" if a.session or config.get("session", {}).get("enabled") else "run"), "--config", str(prepared)]
    if a.state_root:
        command += ["--state-root", str(a.state_root.resolve())]
    raise SystemExit(subprocess.call(command))
