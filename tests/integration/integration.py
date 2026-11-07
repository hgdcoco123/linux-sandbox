"""End-to-end tests for the explicitly unsafe process diagnostic backend."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

BIN = str(Path(sys.argv[1]).resolve())
if sys.platform != "linux":
    raise SystemExit("Linux only")

def require(value, message):
    if not value:
        raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix="sbxt-") as temp:
    root = Path(temp)
    state = root / "s"
    config = root / "job.json"

    def write_config(argv, **options):
        data = {"schema_version": 1, "mode": "process", "network": "host",
                "cwd": "/tmp", "argv": argv, "limits": {"wall_ms": 3000, "grace_ms": 100}}
        data.update(options)
        config.write_text(json.dumps(data))

    def invoke(command, *arguments, timeout=12):
        return subprocess.run([BIN, command, *arguments, "--state-root", str(state)],
                              capture_output=True, timeout=timeout)

    def run(argv, expected="success", **options):
        write_config(argv, **options)
        result = invoke("run", "--config", str(config), "--allow-unsafe")
        try:
            report = json.loads(result.stdout)
        except Exception:
            raise AssertionError(result.stderr.decode(errors="replace"))
        require(report["summary"] == expected, report)
        require(report["cleanup"]["descendants_empty"], report)
        require(report["cleanup"]["launcher_reaped"], report)
        require(report["stdout"]["eof"] and report["stderr"]["eof"], report)
        require(json.loads((state / report["run_id"] / "result.json").read_text()) == report, "report mismatch")
        return report, state / report["run_id"]

    report, directory = run(["/bin/sh", "-c", "printf hello; printf error >&2"])
    require((directory / "stdout.bin").read_bytes() == b"hello", "stdout")
    require((directory / "stderr.bin").read_bytes() == b"error", "stderr")
    # The run hashes what it left on disk so a consumer can prove the bytes it cites.
    import hashlib
    checksums = json.loads((directory / "checksums.json").read_text())
    require(checksums["algorithm"] == "sha256", checksums)
    require(checksums["run_id"] == report["run_id"], checksums)
    for name in ("result.json", "stdout.bin", "stderr.bin"):
        digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        require(checksums["files"][name]["sha256"] == digest, "checksum mismatch: " + name)
        require(checksums["files"][name]["size"] == (directory / name).stat().st_size, checksums)
    # The one-line form must reach the same validator and the same exit codes.
    quick = subprocess.run([BIN, "run", "--process", "--quiet", "--summary", "--",
                            "/bin/sh", "-c", "exit 5"], capture_output=True, text=True)
    require(quick.returncode == 10, "quick run exit code: " + str(quick.returncode))
    require("verdict:" in quick.stdout and "nonzero_exit" in quick.stdout, quick.stdout)
    require("state=" not in quick.stdout, "quiet summary should not contain the live log")
    report, _ = run(["/bin/sh", "-c", "exit 7"], "nonzero_exit")
    require(report["exit_code"] == 7, report)
    run(["/not-existing-program"], "startup_failure")
    run(["/bin/sh", "-c", "kill -SEGV $$"], "signaled")
    run(["/bin/sleep", "20"], "timeout", limits={"wall_ms": 100, "grace_ms": 50})
    run(["/bin/sh", "-c", "trap '' TERM; while :; do :; done"], "timeout",
        limits={"wall_ms": 100, "grace_ms": 50})
    flood = "import os; os.write(1,b'x'*200000); os.write(2,b'y'*200000)"
    report, directory = run([sys.executable, "-c", flood], limits={"capture_bytes": 1024})
    require(report["stdout"]["saved_bytes"] == 1024 and report["stdout"]["discarded_bytes"] == 198976, report)
    orphan = "import os,time,signal; p=os.fork(); (os.setsid(),signal.signal(signal.SIGTERM,signal.SIG_IGN),time.sleep(20)) if p==0 else None"
    run([sys.executable, "-c", orphan])
    sample = root / "sample.bin"
    sample.write_bytes(b"data\0bytes")
    report, directory = run(["/bin/cat"], stdin_file=str(sample))
    require((directory / "stdout.bin").read_bytes() == sample.read_bytes(), "stdin")
    run(["/bin/true"], "startup_failure", cwd=str(root / "absent"))
    write_config(["/bin/true"])
    require(invoke("validate", "--config", str(config)).returncode != 0, "unsafe mode silently accepted")
    require(invoke("validate", "--config", str(config), "--allow-unsafe").returncode == 0, "valid config rejected")

    write_config(["/bin/sleep", "20"])
    before = set(state.iterdir())
    process = subprocess.Popen([BIN, "run", "--config", str(config), "--allow-unsafe", "--state-root", str(state)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        run_id = None
        while time.monotonic() < deadline:
            for path in set(state.iterdir()) - before:
                snapshot = path / "state.json"
                if snapshot.exists() and json.loads(snapshot.read_text())["state"] == "running":
                    run_id = path.name
                    break
            if run_id:
                break
            require(process.poll() is None, "cancel target exited prematurely")
            time.sleep(0.02)
        require(run_id, "run did not become running")
        inspected = invoke("inspect", run_id)
        require(json.loads(inspected.stdout)["active"], "inspect active")
        cancelled = invoke("cancel", run_id)
        require(json.loads(cancelled.stdout)["status"] == "accepted", "cancel not accepted")
        out, err = process.communicate(timeout=8)
        require(json.loads(out)["summary"] == "cancelled", err.decode(errors="replace"))
        require(process.returncode == 130, "cancel CLI code")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    for _ in range(20):
        run(["/bin/true"])
    require(invoke("cleanup").returncode == 0, "cleanup command")
    require(invoke("inspect", "../bad").returncode != 0, "path traversal ID")
print("process integration passed")
