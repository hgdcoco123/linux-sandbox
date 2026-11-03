"""Session preparation is journaled, not best-effort.

A session that cannot be prepared must report *where* it stopped, undo the steps
that already succeeded, and keep the reason it failed separate from any failure of
the undo itself. The state directory is deliberately not undone: the report has to
live somewhere.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binary = str(Path(sys.argv[1]).resolve())


def attempt(state, config_path, config, *extra):
    config_path.write_text(json.dumps(config))
    process = subprocess.run([binary, "session-open", "--config", str(config_path),
                              "--state-root", str(state), *extra],
                             capture_output=True, timeout=30)
    report = json.loads(process.stdout) if process.stdout.strip() else {}
    runs = sorted(p for p in state.glob("run-*") if p.is_dir())
    return process, report, runs[-1] if runs else None


def main():
    with tempfile.TemporaryDirectory(prefix="sbx-prepare-") as temporary:
        root = Path(temporary)
        state = root / "state"
        state.mkdir(mode=0o700)
        config_path = root / "config.json"

        # 1. The last step fails: everything before it must be undone.
        process_config = {
            "schema_version": 1, "mode": "process", "network": "host",
            "argv": ["/bin/true"], "cwd": str(root),
            "limits": {"wall_ms": 2000, "grace_ms": 50},
            "session": {"total_ms": 5000, "idle_ms": 3000},
        }
        process, report, run = attempt(state, config_path, process_config, "--allow-unsafe",
                                       "--launcher", str(root / "no-such-launcher"))
        assert process.returncode == 125, (process.returncode, process.stdout, process.stderr)
        errors = report.get("errors") or []
        assert errors, report
        assert errors[0].startswith("preparation failed at launcher: "), errors
        assert "undone:" in errors[0] and "control_socket" in errors[0], errors
        # The original reason survives; the rollback is reported, not merged into it.
        assert report.get("stop_reason") == "prepare_failed", report.get("stop_reason")
        assert report.get("session_state") == "closed", report.get("session_state")
        assert run is not None, "a run directory is expected even when preparation fails"
        assert not (run / "control.sock").exists(), "control socket survived the rollback"
        assert not (run / "workspace").exists(), "workspace survived the rollback"

        # 2. A middle step fails (cgroup, after the control socket): only the steps
        #    that already completed are undone, and the failing step is named.
        runtime = root / "runtime"
        # The runtime layout is validated up front, so the placeholder directories
        # have to exist even though preparation is expected to fail at the cgroup.
        for name in ("usr/bin", "input", "output", "work", "tmp", "proc", "dev", ".oldroot"):
            (runtime / name).mkdir(parents=True, exist_ok=True)
        not_a_cgroup = root / "not-a-cgroup"
        not_a_cgroup.mkdir()
        isolated_config = {
            "schema_version": 1, "mode": "isolated", "network": "none",
            "argv": ["/usr/bin/sandbox-fixture", "echo"], "cwd": str(root),
            "runtime_root": str(runtime), "cgroup_root": str(not_a_cgroup),
            "limits": {"wall_ms": 2000, "grace_ms": 50},
            "session": {"total_ms": 5000, "idle_ms": 3000},
        }
        process2, report2, run2 = attempt(state, config_path, isolated_config)
        assert process2.returncode == 125, (process2.returncode, process2.stdout, process2.stderr)
        errors2 = report2.get("errors") or []
        assert errors2 and errors2[0].startswith("preparation failed at cgroup: "), errors2
        assert "control_socket" in errors2[0], errors2
        assert report2.get("stop_reason") == "prepare_failed", report2.get("stop_reason")
        assert run2 is not None and not (run2 / "control.sock").exists()
        assert not (run2 / "workspace").exists()

        print("session preparation rollback, failure attribution and stop reason verified")


if __name__ == "__main__":
    main()
