"""Privileged/delegated Linux checks. Skip explicitly unless both paths are supplied."""
from pathlib import Path
import json
import os
import shutil
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
# Not a TemporaryDirectory: a failing case keeps its run directories so the first red
# run is diagnosable (result.json, internal.log, state.json) instead of costing a
# second round trip to reproduce.
temporary = tempfile.mkdtemp(prefix="sbxi-")
root = Path(temporary)


def preserve():
    target = os.environ.get("SBX_TEST_ARTIFACTS")
    if not target:
        return
    for run in sorted((root / "state").glob("run-*")):
        shutil.copytree(run, Path(target) / "isolation" / run.name, dirs_exist_ok=True)
    print(f"kept run state under {target}")


try:
    (root / "input").mkdir()
    (root / "output").mkdir()
    (root / "input/sample.txt").write_text("unchanged")
    config = {"schema_version": 1, "runtime_root": str(Path(runtime).resolve()),
              "cgroup_root": str(Path(cgroup).resolve()), "argv": ["/usr/bin/sandbox-fixture", "files"],
              "mounts": [{"source": str(root / "input"), "target": "/input", "readonly": True},
                         {"source": str(root / "output"), "target": "/output", "readonly": False}],
              "limits": {"wall_ms": 1500, "startup_ms": 5000, "grace_ms": 100}}
    path = root / "job.json"

    def run(mode, expected, limits=None, argv=None, workspace=None, export_ok=True):
        c = dict(config)
        c["argv"] = ["/usr/bin/sandbox-fixture", mode] + (argv or [])
        if limits:
            c["limits"] = dict(config["limits"], **limits)
        if workspace:
            c["workspace"] = workspace
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
        # Decision A: /output is a sandbox-owned tmpfs even for a single run, and the
        # caller's directory is where the final export lands.
        facts = report["workspace"]
        assert facts["mode"] == "tmpfs", facts
        assert facts["export"]["attempted"] is True, facts
        if export_ok is not None:
            assert facts["export"]["ok"] is export_ok, facts
            if export_ok:
                assert facts["export"]["error"] == "", facts
            else:
                assert facts["export"]["error"], facts
        elif not facts["export"]["ok"]:
            # An outcome the sandbox itself cannot survive - an OOM group kill takes
            # init with it today - leaves nothing to hand over. The report must say so
            # instead of pretending; what must never happen is "not ok" with no reason.
            assert facts["export"]["error"] or report["errors"], facts
        return report

    run("proc-view", "success")
    run("files", "success")
    assert (root / "input/sample.txt").read_text() == "unchanged"
    # The workspace only exists inside the sandbox, so the artifact appears in the
    # caller's directory when the run ends - that is the delivery, not a side effect.
    assert (root / "output/result.txt").read_text() == "converted\n"
    run("network", "success")
    run("seccomp", "success")
    run("orphan", "success")
    run("ignore-term", "timeout", {"wall_ms": 100})
    # The memory limit is enforced by the cgroup, and a group OOM kill takes every
    # process in it - init included. So either the empty workspace is handed over
    # before init dies, or the report says it was not; both are honest, silence is not.
    oom = run("memory", "oom", {"memory_bytes": 32 * 1024 * 1024}, export_ok=None)
    assert oom["metrics"]["memory.events.local"]["oom_kill"] > 0, oom
    # The byte budget the session already enforces is the same boundary here: a
    # one-shot run that writes past it gets ENOSPC (the fixture answers 60), and the
    # partial tree is still delivered.
    over = run("fill-bytes", "nonzero_exit", argv=["262144", "big"],
               workspace={"max_bytes": 65536, "keep_on_close": True})
    assert over["exit_code"] == 60, over
    partial = root / "output/big"
    assert partial.is_file() and partial.stat().st_size < 262144, \
        "a run over quota must still hand over what it did write"
    print("single-run quota enforced and the partial tree was exported")
    # An export that cannot be handed over must not read as success: the target exits
    # 0, the handover is refused, and the run is not "ok".
    refused = run("link-output", "host_error", export_ok=False)
    assert refused["verdict"] == "harness_error", refused
    assert refused["errors"], "artifacts that were not delivered must not read as success"
    print("a refused export is reported, not passed off as success")
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
except BaseException:
    preserve()
    raise
else:
    shutil.rmtree(temporary, ignore_errors=True)
print("isolated integration passed")
