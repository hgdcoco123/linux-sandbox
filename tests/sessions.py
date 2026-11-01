"""Linux process-session regression and descriptor-based artifact integration.

Also exercises a real isolated session when SANDBOX_TEST_RUNTIME and SANDBOX_TEST_CGROUP
are explicitly supplied. Process mode is not evidence for isolation correctness.
"""
import hashlib
import array
import json
import os
from pathlib import Path
import subprocess
import socket
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.session_client import SessionClient
from sdk.session_wire import MAGIC, encode_request, decode_response
from sdk import session_v1_pb2 as pb

binary = str(Path(sys.argv[1]).resolve())

def reject(call):
    try:
        call()
    except Exception:
        return
    raise AssertionError("operation was not rejected")

with tempfile.TemporaryDirectory(prefix="sbx-session-") as tmp:
    root = Path(tmp)
    config = root/"process.json"
    config.write_text(json.dumps({"schema_version": 1, "mode": "process", "network": "host",
                                 "argv": ["/bin/true"], "cwd": str(root),
                                 "limits": {"wall_ms": 3000, "grace_ms": 50, "capture_bytes": 4096},
                                 "session": {"total_ms": 60000, "idle_ms": 10000}}))
    client = SessionClient(binary, root/"sdk")
    with client.create(config, allow_unsafe=True) as session:
        def exchange(packet, ancillary=None):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
                peer.settimeout(5)
                peer.connect(str(session.directory/'control.sock'))
                if ancillary:
                    peer.sendmsg([packet], ancillary)
                else:
                    peer.send(packet)
                return peer.recv(16384)
        legacy = json.loads(exchange(json.dumps({'protocol_version': 2, 'op': 'status'}).encode()))
        assert legacy['ok'] and legacy['data']['session_state'] == 'idle'
        assert decode_response(exchange(encode_request({'op': 'status'})))['session_state'] == 'idle'
        bad_version = pb.SessionRequest(protocol_version=99, application_version=2)
        bad_version.status.SetInParent()
        reject(lambda: decode_response(exchange(MAGIC + bad_version.SerializeToString())))
        reject(lambda: decode_response(exchange(MAGIC + b'\xff')))
        with open('/dev/null', 'rb') as extra:
            rights = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [extra.fileno()]))]
            reject(lambda: decode_response(exchange(encode_request({'op': 'status'}), rights)))
        # Deliberately lose the first execute response, then retry the same ID.
        request = {"protocol_version": 2, "op": "execute", "request_id": "lost-response",
                   "argv": ["/bin/sh", "-c", "printf x >> dedup-count; sleep 0.1"]}
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
            peer.connect(str(session.directory/'control.sock'))
            peer.send(json.dumps(request).encode())
        deadline = time.monotonic() + 5
        while not list(session.directory.glob('cmd-*/request.json')):
            assert time.monotonic() < deadline
            time.sleep(0.01)
        retried = session.execute(request['argv'], request_id='lost-response')
        assert retried.wait(5)['exit_code'] == 0
        again = session.execute(request['argv'], request_id='lost-response')
        assert again.command_id == retried.command_id
        assert (root/'dedup-count').read_text() == 'x'
        assert session.request_status('lost-response')['state'] == 'finished'
        legacy_status = json.loads(exchange(json.dumps({'protocol_version':2, 'op':'request-status',
                                                        'request_id':'lost-response'}).encode()))
        assert legacy_status['ok'] and legacy_status['data']['command_id'] == retried.command_id
        reject(lambda: session.execute(['/bin/false'], request_id='lost-response'))
        reject(lambda: session.execute(['/bin/true'], wall_ms=0))
        first = session.execute(["/bin/sh", "-c", "printf hello > value; printf '\\000\\377'"])
        assert first.wait(5)["exit_code"] == 0
        assert first.output()["bytes"] == b"\0\xff"
        assert first.output(offset=1)['bytes'] == b'\xff'
        second = session.execute(["/bin/cat", "value"])
        assert second.wait(5)["exit_code"] == 0
        assert second.output()["bytes"] == b"hello"
        assert session.execute(["/bin/sh", "-c", "exit 7"]).wait(5)["exit_code"] == 7
        assert session.execute(["/not-existing"]).wait(5)["exec_errno"] is not None
        slow = session.execute(["/bin/sleep", "20"])
        reject(lambda: session.execute(["/bin/true"]))
        try:
            slow.wait(0.01)
            raise AssertionError("wait should time out")
        except TimeoutError:
            pass
        assert session.status()["current"]["command_id"] == slow.command_id
        slow.cancel()
        assert slow.wait(5)["stop_reason"] == "cancelled"
        timeout = session.execute(["/bin/sleep", "20"], wall_ms=50)
        assert timeout.wait(5)["stop_reason"] == "timeout"
        cat = session.execute(["/bin/cat"])
        assert cat.write(b"input\0bytes") == 11
        cat.close_stdin()
        assert cat.wait(5)["exit_code"] == 0
        assert cat.output()["bytes"] == b"input\0bytes"
        reject(lambda: first.cancel())  # An old command handle cannot cancel a newer command.
        flood = session.execute(["/bin/sh", "-c", "head -c 20000 /dev/zero"])
        result = flood.wait(5)
        assert result["stdout"]["stored"] == 4096 and result["stdout"]["seen"] == 20000
        assert result['stdout']['flushed'] and result['stderr']['flushed']
        assert result['stdout']['queue_peak_bytes'] <= 256*1024
        assert session.execute(["/bin/true"]).wait(5)["exit_code"] == 0
        attached = client.attach(session.state_root, session.run_id)
        assert attached.status()["session_state"] == "idle"
    assert session.status()["session_state"] == "closed"
    assert session.process.returncode == 0
    assert session.request_status('lost-response')['state'] == 'finished'

    # A long-lived session must not accumulate resources per command. Descriptors and
    # child processes are exact signals: a leak of one descriptor per command would
    # show up as +100 here. Resident memory is inherently coarse (the allocator may
    # hold freed buffers), so it gets a bound rather than an equality - it catches
    # leaks of tens of kilobytes per command, not single bytes.
    with client.create(config, allow_unsafe=True) as session:
        def inventory():
            pid = json.loads((session.directory/"owner.json").read_text())["supervisor_pid"]
            descriptors = len(os.listdir(f"/proc/{pid}/fd"))
            children = len(Path(f"/proc/{pid}/task/{pid}/children").read_text().split())
            page_kb = os.sysconf("SC_PAGE_SIZE") // 1024
            resident = int(Path(f"/proc/{pid}/statm").read_text().split()[1]) * page_kb
            return descriptors, children, resident

        for _ in range(10):  # warm up: first commands allocate the steady-state set
            assert session.execute(["/bin/true"]).wait(5)["exit_code"] == 0
        base = inventory()
        for _ in range(100):
            command = session.execute(["/bin/sh", "-c", "printf x"])
            assert command.wait(5)["exit_code"] == 0
        # wait() returns when the result is written, which is a moment before the
        # supervisor releases that command's captures. Sample the settled state, not
        # the transient one.
        deadline = time.monotonic() + 5
        after = inventory()
        while time.monotonic() < deadline and after[0] > base[0] + 2:
            time.sleep(0.05)
            after = inventory()
        assert after[0] <= base[0] + 2, (base, after)
        assert after[1] == base[1], (base, after)
        assert after[2] <= base[2] + 4096, (base, after)
    print("session resource growth bounded over 100 commands")
    # File policy runs through the same CLI and C++ implementation used by sessions.
    source = root/"input"
    source.mkdir()
    (source/"data").write_bytes(b"snapshot\0payload")
    def files(op, **args):
        request = root/"file-request.json"
        request.write_text(json.dumps({"op": op, **args}))
        p = subprocess.run([binary, "files", "--request", str(request)], capture_output=True, timeout=10)
        if p.returncode:
            raise RuntimeError(p.stderr.decode())
        return json.loads(p.stdout)
    copied = root/"copied"
    manifest = files("snapshot", source=str(source), destination=str(copied))
    assert manifest["entries"][0]["sha256"] == hashlib.sha256(b"snapshot\0payload").hexdigest()
    (source/"data").write_bytes(b"changed")
    assert (copied/"data").read_bytes() == b"snapshot\0payload"
    reject(lambda: files("export", source=str(source), destination=str(copied)))
    (source/"link").symlink_to("/etc/passwd")
    reject(lambda: files("manifest", source=str(source)))
    (source/"link").unlink()
    os.link(source/"data", source/"hardlink")
    reject(lambda: files("manifest", source=str(source)))
    print("process sessions and artifact regression passed")

    runtime = os.environ.get("SANDBOX_TEST_RUNTIME")
    cgroup = os.environ.get("SANDBOX_TEST_CGROUP")
    if runtime and cgroup:
        isolated = root/"isolated.json"
        isolated.write_text(json.dumps({"schema_version": 1, "argv": ["/usr/bin/sandbox-fixture", "echo"],
                                       "runtime_root": runtime, "cgroup_root": cgroup,
                                       "session": {"total_ms": 30000, "idle_ms": 10000}}))
        with client.create(isolated) as session:
            upload = root/"upload"
            upload.mkdir()
            (upload/"hello").write_text("world")
            session.upload(upload, "input")
            for _ in range(2):
                command = session.execute(["/usr/bin/sandbox-fixture", "echo"])
                assert command.wait(5)["exit_code"] == 0
                proc = session.execute(["/usr/bin/sandbox-fixture", "proc-view"])
                assert proc.wait(5)["exit_code"] == 0
            for mode in ("session-write", "session-read", "session-copy-executable"):
                assert session.execute(["/usr/bin/sandbox-fixture", mode]).wait(5)["exit_code"] == 0
            assert session.execute(["/output/copied-probe", "echo"]).wait(5)["exit_code"] == 0
            exported = root/"exported"
            session.export(exported)
            assert (exported/"input"/"hello").read_text() == "world"
        print("isolated session smoke passed")

        # /input as a snapshot: the copy is taken when the session starts, so a
        # host edit afterwards must not be visible inside the sandbox. The
        # assertion reads the durable copy from the host side, which is exactly
        # the guarantee, and does not depend on any fixture mode.
        source = root/"snapshot-source"
        source.mkdir()
        (source/"value").write_text("original")
        snapshot_config = root/"isolated-snapshot.json"
        snapshot_config.write_text(json.dumps({
            "schema_version": 1, "argv": ["/usr/bin/sandbox-fixture", "echo"],
            "runtime_root": runtime, "cgroup_root": cgroup,
            "workspace": {"input": "snapshot_copy"},
            "mounts": [{"source": str(source), "target": "/input", "readonly": True}],
            "session": {"total_ms": 15000, "idle_ms": 8000}}))
        with client.create(snapshot_config) as session:
            (source/"value").write_text("changed")
            copied = (session.directory/"input"/"value").read_text()
            assert copied == "original", copied
        print("isolated /input snapshot passed")
    else:
        print("isolated session smoke NOT EXECUTED: supply SANDBOX_TEST_RUNTIME/SANDBOX_TEST_CGROUP")
