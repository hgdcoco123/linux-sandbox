"""Persistent sessions. All execution and file policy remains in the C++ CLI.

Dropping a handle does not destroy the session. close() does; wait(timeout) only
stops waiting. Byte offsets apply independently to stdout and stderr.
"""
from dataclasses import dataclass
from pathlib import Path
import json
import array
import os
import re
import socket
import stat
import struct
import subprocess
import tempfile
import time
import uuid

from .sandbox_client import SandboxClient, SandboxError


@dataclass
class CommandHandle:
    session: "SessionHandle"
    command_id: str
    request_id: str = None

    def result(self, _timeout=5):
        saved = self.session.directory / self.command_id / "result.json"
        if saved.exists():
            return json.loads(saved.read_text(encoding="utf-8"))
        return self.session.request(op="result", command_id=self.command_id, _timeout=_timeout)

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            saved = self.session.directory / self.command_id / "result.json"
            if saved.exists():
                result = json.loads(saved.read_text(encoding="utf-8"))
                if result["state"] in ("finished", "interrupted"):
                    return result
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                raise TimeoutError("wait expired; command remains running")
            try:
                result = self.result(_timeout=remaining if remaining is not None else 5)
            except socket.timeout as error:
                raise TimeoutError("wait expired; command remains running") from error
            if result["state"] in ("finished", "interrupted"):
                return result
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("wait expired; command remains running")
            time.sleep(0.02 if deadline is None else min(0.02, max(0, deadline-time.monotonic())))

    def output(self, stream="stdout", offset=0):
        if stream not in ("stdout", "stderr") or offset < 0:
            raise ValueError("invalid stream or offset")
        if self.session.status()["session_state"] == "closed":
            with (self.session.directory/self.command_id/(stream+".bin")).open("rb") as file:
                file.seek(offset)
                data = file.read(2048)
            return {"offset": offset, "next_offset": offset+len(data), "bytes": data, "finished": True}
        try:
            value = self.session.request(op="output", command_id=self.command_id,
                                         stream=stream, offset=offset)
        except (SandboxError, socket.timeout):
            if self.session._saved_status()["session_state"] != "closed":
                raise
            with (self.session.directory/self.command_id/(stream+".bin")).open("rb") as file:
                file.seek(offset)
                data = file.read(2048)
            return {"offset": offset, "next_offset": offset+len(data), "bytes": data, "finished": True}
        value["bytes"] = bytes(value["bytes"])
        return value

    def write(self, data):
        if not isinstance(data, bytes) or len(data) > 4096:
            raise ValueError("write requires at most 4096 bytes")
        status = self.session.status()
        if not status["current"] or status["current"]["command_id"] != self.command_id:
            raise SandboxError("command is no longer active")
        return self.session.request(op="stdin", command_id=self.command_id, bytes=list(data))["accepted_bytes"]

    def close_stdin(self):
        return self.session.request(op="stdin-close", command_id=self.command_id)

    def cancel(self):
        return self.session.request(op="cancel", command_id=self.command_id)


@dataclass
class SessionHandle:
    client: "SessionClient"
    state_root: Path
    run_id: str
    process: object = None

    @property
    def directory(self):
        return self.state_root / self.run_id

    def _offline_request_status(self, request):
        name = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", suffix=".json",
                                             dir=self.state_root, delete=False) as file:
                name = Path(file.name)
                json.dump({"protocol_version": 2, **request}, file)
            return self.client._command("session-request", self.run_id, "--request", str(name),
                                        state_root=self.state_root)
        finally:
            if name is not None:
                name.unlink(missing_ok=True)

    def request(self, _timeout=5, **request):
        # Import lazily so users of the one-shot and batch APIs do not require
        # Python protobuf merely to import their clients.
        from .session_wire import encode_request, decode_response, MAX_PACKET
        if not re.fullmatch(r"run-[0-9a-f]{24}", self.run_id):
            raise SandboxError("invalid run ID", code="INVALID_REQUEST")
        try:
            for directory in (self.state_root, self.directory):
                info = directory.lstat()
                if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                    raise SandboxError("state directory must be user-owned and private", code="INVALID_REQUEST")
            if request.get("op") == "request-status" and self._saved_status().get("session_state") == "closed":
                return self._offline_request_status(request)
            packet = encode_request(request)
            deadline = None if _timeout is None else time.monotonic() + _timeout
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as channel:
                def apply_deadline():
                    if deadline is not None:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise socket.timeout("session request deadline exceeded")
                        channel.settimeout(remaining)
                apply_deadline()
                channel.connect(str(self.directory / "control.sock"))
                peer = channel.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
                if struct.unpack("3i", peer)[1] != os.getuid():
                    raise SandboxError("session peer has unexpected UID", code="TRANSPORT_ERROR")
                apply_deadline()
                if channel.send(packet) != len(packet):
                    raise SandboxError("short session request", code="TRANSPORT_ERROR")
                apply_deadline()
                data, control, flags, _ = channel.recvmsg(MAX_PACKET, socket.CMSG_SPACE(16 * array.array("i").itemsize))
                for level, kind, payload in control:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        fds = array.array("i")
                        fds.frombytes(payload[:len(payload) - len(payload) % fds.itemsize])
                        for fd in fds:
                            os.close(fd)
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or control or not data:
                    raise SandboxError("invalid or truncated session response", code="PROTOCOL_ERROR")
                return decode_response(data)
        except socket.timeout:
            raise
        except OSError as error:
            # request-status is read-only; the CLI also verifies the durable
            # run and only reconciles from disk after the host has exited.
            if request.get("op") == "request-status":
                return self._offline_request_status(request)
            raise SandboxError(str(error), code="TRANSPORT_ERROR") from error

    def _saved_status(self):
        saved = self.directory / "session.json"
        return json.loads(saved.read_text(encoding="utf-8"))

    def status(self):
        state = self._saved_status()
        if state["session_state"] == "closed":
            return state
        try:
            return self.request(op="status")
        except (SandboxError, socket.timeout):
            # The host can publish closed and remove the socket after our first
            # disk read, while a status request is still pending. Trust only a
            # confirmed durable terminal state; retain genuine connection errors.
            state = self._saved_status()
            if state["session_state"] == "closed":
                return state
            raise

    def execute(self, argv, cwd=None, env=None, wall_ms=None, request_id=None):
        request = {"op": "execute", "argv": list(argv),
                   "request_id": uuid.uuid4().hex if request_id is None else request_id}
        request.update({key: value for key, value in
                        (("cwd", cwd), ("env", env), ("wall_ms", wall_ms)) if value is not None})
        try:
            result = self.request(**request)
        except Exception as error:
            # Preserve the correlation key even when the first response is lost.
            error.request_id = request["request_id"]
            raise
        return CommandHandle(self, result["command_id"], request["request_id"])

    def request_status(self, request_id):
        return self.request(op="request-status", request_id=request_id)

    def upload(self, source, name="input"):
        return self.request(op="files", request={"op": "snapshot", "source": str(Path(source).resolve()), "name": name})

    def manifest(self):
        return self.request(op="files", request={"op": "manifest"})

    def export(self, destination):
        return self.request(op="files", request={"op": "export", "destination": str(Path(destination).absolute())})

    def close(self, timeout=10):
        deadline = time.monotonic() + timeout
        if self._saved_status()["session_state"] != "closed":
            try:
                self.request(op="close", _timeout=max(0, deadline-time.monotonic()))
            except socket.timeout as error:
                if self._saved_status()["session_state"] != "closed":
                    raise TimeoutError("session close deadline exceeded") from error
            except SandboxError:
                if self._saved_status()["session_state"] != "closed":
                    raise
        # Closing ends the control server. Observe atomic disk snapshots instead
        # of issuing more RPCs to a service which is intentionally shutting down.
        state = self._saved_status()
        while state["session_state"] != "closed":
            if self.process is not None and self.process.poll() is not None:
                state = self._saved_status()
                if state["session_state"] == "closed":
                    break
                raise SandboxError("session host exited without a durable closed state")
            if time.monotonic() >= deadline:
                raise TimeoutError("session close deadline exceeded")
            time.sleep(0.02)
            state = self._saved_status()
        if self.process is not None:
            self.process.wait(timeout=max(0.1, deadline-time.monotonic()))
        return state

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            if exc_value is None:
                raise
            # Preserve the actual failure in the with body when cleanup also fails.
            note = f"Session cleanup also failed: {cleanup_error}"
            if hasattr(exc_value, "add_note"):
                exc_value.add_note(note)
            else:
                import warnings
                warnings.warn(note, RuntimeWarning)
        return False


class SessionClient(SandboxClient):
    def attach(self, state_root, run_id):
        # Ask C++ to validate the ID and directory before constructing a handle.
        root = Path(state_root).resolve()
        self._command("inspect", run_id, state_root=root)
        return SessionHandle(self, root, run_id)

    def create(self, config_file, allow_unsafe=False, readiness_timeout=15, startup="auto"):
        root = Path(tempfile.mkdtemp(prefix="session-", dir=self.state_root))
        from .launch import plan_launch
        plan = plan_launch(self.binary, config_file, root, session=True,
                           allow_unsafe=allow_unsafe, startup=startup)
        command = plan.argv
        # Spool output: a detached client must not leave unread PIPEs that block the host.
        with (root/"host.stdout").open("wb") as out, (root/"host.stderr").open("wb") as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
        deadline = time.monotonic() + readiness_timeout
        while time.monotonic() < deadline:
            for file in root.glob("run-*/session.json"):
                state = json.loads(file.read_text(encoding="utf-8"))
                if state["session_state"] == "idle":
                    return SessionHandle(self, root, file.parent.name, process)
            if process.poll() is not None:
                raise SandboxError((root/"host.stderr").read_text(errors="replace") +
                                   (root/"host.stdout").read_text(errors="replace"))
            time.sleep(0.02)
        plan.stop(process)
        raise SandboxError("session readiness deadline exceeded")
