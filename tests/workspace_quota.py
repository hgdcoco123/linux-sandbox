"""Runtime workspace quota, export and failure reporting.

The session workspace is a tmpfs owned by the sandbox's mount namespace, so the
supervisor cannot see it: the host copy is a published snapshot, handed over on
request. These cases only run under a delegated cgroup with a runtime, because
that is where the tmpfs exists at all.

Skipping is not passing: every case asserts something specific.
"""
import json
import os
import signal
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.session_client import SessionClient
from sdk.sandbox_client import SandboxError

runtime = os.environ.get("SANDBOX_TEST_RUNTIME")
cgroup = os.environ.get("SANDBOX_TEST_CGROUP")
if not runtime or not cgroup:
    print("SKIP: set SANDBOX_TEST_RUNTIME and SANDBOX_TEST_CGROUP after environment setup")
    raise SystemExit(77)
binary = str(Path(sys.argv[1]).resolve())
fixture = "/usr/bin/sandbox-fixture"


def config_for(workspace):
    return {
        "schema_version": 1, "argv": [fixture, "echo"],
        "runtime_root": str(Path(runtime).resolve()), "cgroup_root": str(Path(cgroup).resolve()),
        "workspace": workspace,
        "session": {"total_ms": 30000, "idle_ms": 10000},
    }


def facts_of(session):
    report = json.loads((session.directory / "result.json").read_text())
    return report.get("workspace", {}), report


with tempfile.TemporaryDirectory(prefix="sbx-quota-") as temporary:
    root = Path(temporary)
    client = SessionClient(binary, root / "sdk")

    # 1. Byte budget: a write past max_bytes fails with ENOSPC (the fixture returns
    #    60 for that), and the session survives it.
    config = root / "bytes.json"
    config.write_text(json.dumps(config_for({"max_bytes": 65536, "keep_on_close": True})))
    session = client.create(config)
    over = session.execute([fixture, "fill-bytes", "262144"]).wait(15)
    assert over["exit_code"] == 60, over
    # Within the budget the same command succeeds: the limit is a ceiling, not a ban.
    assert session.execute([fixture, "fill-bytes", "1024"]).wait(10)["exit_code"] == 0
    exported = root / "bytes-export"
    session.export(exported)
    session.close()
    facts, report = facts_of(session)
    assert facts.get("mode") == "tmpfs", facts
    assert facts.get("quota", {}).get("max_bytes") == 65536, facts
    assert report.get("errors") == [], report
    print("byte quota enforced; the session survived and exported")

    # 2. Inode budget: max_entries bounds the number of files, not their size.
    config = root / "inodes.json"
    config.write_text(json.dumps(config_for({"max_entries": 8})))
    with client.create(config) as session:
        assert session.execute([fixture, "fill-files", "40"]).wait(15)["exit_code"] == 60
        assert session.execute([fixture, "fill-files", "3"]).wait(10)["exit_code"] == 0
    print("inode quota enforced")

    # 3. Export failure: when the handover cannot happen the answer must say so,
    #    rather than presenting a stale copy as the current workspace.
    config = root / "export.json"
    config.write_text(json.dumps(config_for({"keep_on_close": True})))
    session = client.create(config)
    assert session.execute([fixture, "session-write"]).wait(10)["exit_code"] == 0
    transfer = session.directory / "transfer"
    original = transfer.stat().st_mode
    os.chmod(transfer, 0o500)
    try:
        blocked = False
        try:
            session.export(root / "blocked-export")
        except SandboxError:
            blocked = True
        assert blocked, "an export that cannot be handed over must not succeed"
    finally:
        os.chmod(transfer, original)
    session.close()
    facts, report = facts_of(session)
    assert facts.get("export", {}).get("ok") is False, facts
    assert facts.get("export", {}).get("error"), facts
    print("a blocked export is reported as an export failure")

    # 4. init dying mid-command: artifacts may be undeliverable, but the session must
    #    not claim success.
    config = root / "init-death.json"
    config.write_text(json.dumps(config_for({"keep_on_close": True})))
    session = client.create(config)
    session.execute([fixture, "sleep"])
    time.sleep(0.3)
    init_pid = json.loads((session.directory / "owner.json").read_text())["init_pid"]
    os.kill(init_pid, signal.SIGKILL)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and session.process.poll() is None:
        time.sleep(0.05)
    assert session.process.poll() is not None, "the session should end when init dies"
    facts, report = facts_of(session)
    assert report.get("errors"), report
    assert facts.get("export", {}).get("ok") is not True, facts
    print("init death is reported, not passed off as success")

print("workspace quota, export and failure reporting passed")
