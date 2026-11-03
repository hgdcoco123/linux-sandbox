"""Runtime workspace quota, export and failure reporting.

The session workspace is a tmpfs owned by the sandbox's mount namespace, so the
supervisor cannot see it: the host copy is a published snapshot, handed over on
request. These cases only run under a delegated cgroup with a runtime, because
that is where the tmpfs exists at all.

Skipping is not passing: every case asserts something specific.
"""
import json
import os
import shutil
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk.session_client import SessionClient
from sdk.sandbox_client import SandboxError

runtime = os.environ.get("SANDBOX_TEST_RUNTIME")
cgroup = os.environ.get("SANDBOX_TEST_CGROUP")
if not runtime or not cgroup:
    print("SKIP: set SANDBOX_TEST_RUNTIME and SANDBOX_TEST_CGROUP after environment setup")
    raise SystemExit(77)
binary = str(Path(sys.argv[1]).resolve())
fixture = "/usr/bin/sandbox-fixture"

# When a case fails, the run directory is the only place that explains why: it holds
# result.json, internal.log and host.stderr. Python deletes them with the temporary
# directory on the way out, so snapshot them first.
#
# The session state root lives *outside* that temporary directory on purpose: the failure
# hook runs after `with TemporaryDirectory` has already cleaned up, so a state root inside
# it was gone by the time it was copied - the hook printed "kept session state" while
# there was nothing left to copy.
sdk_state = Path(tempfile.mkdtemp(prefix="sbx-quota-sdk-"))
_tracked = []


def _preserve():
    target = os.environ.get("SBX_TEST_ARTIFACTS")
    if not target:
        return
    for index, directory in enumerate(_tracked):
        if not directory.exists():
            continue
        shutil.copytree(directory, Path(target) / f"session-{index}", dirs_exist_ok=True)
    print(f"kept session state under {target}")


def _hook(kind, value, trace):
    _preserve()
    sys.__excepthook__(kind, value, trace)


sys.excepthook = _hook


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
    client = SessionClient(binary, sdk_state)

    # 1. Byte budget: a write past max_bytes fails with ENOSPC (the fixture returns
    #    60 for that), and the session survives it.
    config = root / "bytes.json"
    config.write_text(json.dumps(config_for({"max_bytes": 65536, "keep_on_close": True})))
    session = client.create(config)
    _tracked.append(session.directory)
    # Within the budget first: the limit is a ceiling, not a ban. Order matters
    # because the quota is cumulative - once the session has filled the workspace,
    # later writes fail no matter how small they are.
    assert session.execute([fixture, "fill-bytes", "1024", "small"]).wait(10)["exit_code"] == 0
    over = session.execute([fixture, "fill-bytes", "262144", "big"]).wait(15)
    assert over["exit_code"] == 60, over
    # The transfer channel is a descriptor, not a path: nothing of the host directory
    # exists in the target's filesystem, so it cannot write past the quota or touch
    # staged data.
    probe = session.execute([fixture, "transfer-probe"]).wait(10)["exit_code"]
    assert probe == 0, "the target could write into the transfer channel"
    # The session itself survives the quota: a command that writes nothing still runs.
    assert session.execute([fixture, "echo"]).wait(10)["exit_code"] == 0
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
        _tracked.append(session.directory)
        # Same order rule: a few files fit, many do not.
        assert session.execute([fixture, "fill-files", "3"]).wait(10)["exit_code"] == 0
        assert session.execute([fixture, "fill-files", "40"]).wait(15)["exit_code"] == 60
    print("inode quota enforced")

    # 3. Export failure: when the handover cannot happen the answer must say so,
    #    rather than presenting a stale copy as the current workspace.
    config = root / "export.json"
    config.write_text(json.dumps(config_for({"keep_on_close": True})))
    session = client.create(config)
    _tracked.append(session.directory)
    assert session.execute([fixture, "session-write"]).wait(10)["exit_code"] == 0
    # One successful handover first: that is the copy a later failure must not destroy.
    session.export(root / "first-export")
    published = session.directory / "workspace" / "value"
    assert published.read_text() == "persistent", "the first export was not published"
    transfer = session.directory / "transfer"
    original = transfer.stat().st_mode
    try:
        os.chmod(transfer, 0o500)
        blocked = False
        try:
            session.export(root / "blocked-export")
        except SandboxError:
            blocked = True
        assert blocked, "an export that cannot be handed over must not succeed"
        # Publishing is an exchange: until it happens, the previous copy is intact.
        assert published.read_text() == "persistent", \
            "a failed export destroyed the previous published copy"
        # Keep the handover blocked until the session closes: with keep_on_close the
        # session promised to keep the artifacts, and it could not deliver them.
        try:
            session.close()
        except SandboxError:
            pass  # a failed delivery is reported through the result, not by close()
        facts, report = facts_of(session)
    finally:
        os.chmod(transfer, original)
    assert facts.get("export", {}).get("ok") is False, facts
    assert facts.get("export", {}).get("error"), facts
    assert report.get("errors"), "artifacts that were not delivered must not read as success"
    print("a blocked export is reported as an export failure")

    # 4. init dying mid-command: artifacts may be undeliverable, but the session must
    #    not claim success.
    config = root / "init-death.json"
    config.write_text(json.dumps(config_for({"keep_on_close": True})))
    session = client.create(config)
    _tracked.append(session.directory)
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

    # 5. A transfer that never answers is reported, and it poisons the channel: the
    #    copy may still be running, so a second request must not be answered by the
    #    first one's reply.
# Zero, not one: giving the sandbox no time at all makes the timeout certain,
    # where a 1ms budget only loses the race when the copy is slow enough.
    os.environ["SBX_TRANSFER_WAIT_MS"] = "0"
    try:
        config = root / "timeout.json"
        config.write_text(json.dumps(config_for({"keep_on_close": True})))
        session = client.create(config)
        _tracked.append(session.directory)
        assert session.execute([fixture, "fill-files", "20"]).wait(10)["exit_code"] == 0
        timed_out = False
        try:
            session.export(root / "late-export")
        except SandboxError as failure:
            timed_out = "answer" in str(failure)
        assert timed_out, "a transfer that never answers must be reported as such"
        refused = False
        try:
            session.export(root / "second-export")
        except SandboxError as failure:
            # It must be refused *because the channel is quarantined*, not for some
            # unrelated reason that would also raise.
            refused = "timed out" in str(failure) or "still be copying" in str(failure)
        assert refused, "a quarantined transfer channel must not be reused"
        # The abandoned request's answer still arrives later: the copy kept running inside
        # the sandbox. The main loop is the only reader left at that point, and it used to
        # treat that answer as a protocol error and kill a healthy session. Give it time to
        # arrive, then prove the session is still usable.
        time.sleep(0.5)
        assert session.execute([fixture, "echo"]).wait(10)["exit_code"] == 0, \
            "a late transfer reply must be discarded, not treated as a protocol error"
        session.close()
        facts, report = facts_of(session)
        assert facts.get("export", {}).get("ok") is False, facts
    finally:
        del os.environ["SBX_TRANSFER_WAIT_MS"]
    print("a timed-out transfer is reported and the channel is quarantined")

    # 6. An external stop must reach an in-flight copy. The supervisor waits for the
    #    sandbox to answer; before this, a signal arriving during that wait was only
    #    noticed after the 30s transfer budget, so the caller's stop appeared ignored.
    #    This is the end-to-end counterpart of publish_tests' "abort mid copy": the
    #    request really comes from outside, not from an injected callback.
    config = root / "stop-during-export.json"
    # Many entries, because the copy loop can only stop *between* entries: a single
    # huge file would not be interruptible, which is a separate (documented) limit.
    config.write_text(json.dumps(config_for({"max_entries": 20000, "keep_on_close": True})))
    session = client.create(config)
    _tracked.append(session.directory)
    assert session.execute([fixture, "fill-files", "15000"]).wait(60)["exit_code"] == 0
    outcome = {}

    def export_in_background():
        try:
            session.export(root / "stopped-export")
        except Exception as error:  # the interruption is the expected answer
            outcome["error"] = error

    worker = threading.Thread(target=export_in_background)
    started = time.monotonic()
    worker.start()
    time.sleep(0.1)  # the copy of 15000 entries is still running at this point
    session.process.send_signal(signal.SIGINT)
    worker.join(30)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and session.process.poll() is None:
        time.sleep(0.02)
    elapsed = time.monotonic() - started
    assert session.process.poll() is not None, \
        "an external stop during the export must end the session"
    assert elapsed < 5, f"the stop waited out the transfer budget ({elapsed:.1f}s)"
    assert "error" in outcome, "the copy should have been interrupted, not completed"
    assert getattr(outcome["error"], "code", "") == "TRANSFER_INTERRUPTED", outcome["error"]
    facts, report = facts_of(session)
    assert report.get("stop_reason") == "signal", report
    assert facts.get("export", {}).get("ok") is False, facts
    print("an external stop interrupts an in-flight export")

# Only reached when every case passed; on failure the state root is left for the hook.
shutil.rmtree(sdk_state, ignore_errors=True)
print("workspace quota, export and failure reporting passed")
