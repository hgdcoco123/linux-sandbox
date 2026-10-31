"""Privileged/delegated Linux checks. Skip explicitly unless both paths are supplied."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import time

runtime = os.environ.get("SANDBOX_TEST_RUNTIME")
cgroup = os.environ.get("SANDBOX_TEST_CGROUP")
if not runtime or not cgroup:
    print("SKIP: set SANDBOX_TEST_RUNTIME and SANDBOX_TEST_CGROUP after environment setup")
    raise SystemExit(77)
binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="sbxi-") as temp:
    root = Path(temp)
    (root / "input").mkdir()
    (root / "output").mkdir()
    (root / "input/sample.txt").write_text("unchanged")
    config = {"schema_version": 1, "runtime_root": str(Path(runtime).resolve()),
              "cgroup_root": str(Path(cgroup).resolve()), "argv": ["/usr/bin/sandbox-fixture", "files"],
              "mounts": [{"source": str(root / "input"), "target": "/input", "readonly": True},
                         {"source": str(root / "output"), "target": "/output", "readonly": False}],
              "limits": {"wall_ms": 1500, "startup_ms": 5000, "grace_ms": 100}}
    path = root / "job.json"
    def run(mode, expected, limits=None):
        c = dict(config)
        c["argv"] = ["/usr/bin/sandbox-fixture", mode]
        if limits:
            c["limits"] = dict(config["limits"], **limits)
        path.write_text(json.dumps(c))
        result = subprocess.run([binary, "run", "--config", str(path), "--state-root", str(root / "state")],
                                capture_output=True, timeout=15)
        try:
            report = json.loads(result.stdout)
        except Exception:
            raise AssertionError(result.stderr.decode(errors="replace"))
        assert report["summary"] == expected, report
        assert report["cleanup"]["descendants_empty"] and report["cleanup"]["cgroup_removed"], report
        assert report["metrics"]["sampling"]["samples_taken"] >= 1, report
        assert len(report["metrics"]["sampling"]["history"]) <= report["metrics"]["sampling"]["history_limit"], report
        return report
    run("proc-view", "success")
    run("files", "success")
    assert (root / "input/sample.txt").read_text() == "unchanged"
    assert (root / "output/result.txt").read_text() == "converted\n"
    run("network", "success")
    run("seccomp", "success")
    run("orphan", "success")
    run("ignore-term", "timeout", {"wall_ms": 100})
    oom = run("memory", "oom", {"memory_bytes": 32 * 1024 * 1024})
    assert oom["metrics"]["memory.events.local"]["oom_kill"] > 0, oom
    # A live inspect must show periodic per-run metrics, not just final counters.
    active = dict(config)
    active["argv"] = ["/usr/bin/sandbox-fixture", "sleep"]
    active["limits"] = dict(config["limits"], wall_ms=1200)
    active["metrics"] = {"interval_ms": 100, "history_limit": 4}
    path.write_text(json.dumps(active))
    process = subprocess.Popen([binary, "run", "--config", str(path), "--state-root", str(root / "state")],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        observed = False
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            for snapshot in (root / "state").glob("run-*/state.json"):
                state = json.loads(snapshot.read_text())
                latest = state.get("metrics", {}).get("latest")
                if state["state"] == "running" and latest and latest["elapsed_ms"] >= 100:
                    response = subprocess.run([binary, "inspect", state["run_id"], "--state-root", str(root / "state")],
                                              capture_output=True, check=True, timeout=3)
                    assert json.loads(response.stdout)["state"]["metrics"]["samples_taken"] >= 2
                    observed = True
                    break
            if observed:
                break
            time.sleep(0.02)
        stdout, stderr = process.communicate(timeout=10)
        report = json.loads(stdout)
        assert observed, stderr.decode(errors="replace")
        assert report["summary"] == "timeout", report
        assert report["metrics"]["sampling"]["samples_taken"] >= 3, report
        assert len(report["metrics"]["sampling"]["history"]) <= 4, report
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=8)
print("isolated integration passed")
