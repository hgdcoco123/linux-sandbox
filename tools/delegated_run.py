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
p.add_argument("--artifacts", type=Path,
               help="directory that failing cases copy their session state into")
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
                ". No policy was changed. See docs/INSTALL.md; full doctor output: " +
                json.dumps(probe, ensure_ascii=False))
# The cgroup layout lives in one module so the AI end-to-end driver prepares exactly the
# same subtree instead of keeping a copy that can drift.
from delegated_cgroup import prepare

try:
    runs = prepare()
except RuntimeError as error:
    p.error(str(error))
binary = str(a.sandbox.resolve())
if a.test_runtime:
    env = dict(os.environ, SANDBOX_TEST_RUNTIME=str(a.test_runtime.resolve()), SANDBOX_TEST_CGROUP=str(runs))
    # The cases copy their state here when they fail; passing the flag without this
    # export is what makes a red run need a second round trip to diagnose.
    if a.artifacts:
        env["SBX_TEST_ARTIFACTS"] = str(a.artifacts.resolve())
    for test in ("acceptance/isolation.py", "integration/sessions.py", "acceptance/workspace_quota.py", "integration/batch_integration.py"):
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
