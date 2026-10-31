"""A process-based SDK: no Linux isolation logic is duplicated here."""
from dataclasses import dataclass
from pathlib import Path
import json
import subprocess
import tempfile
import time

class SandboxError(RuntimeError):
    def __init__(self, message, code="OPERATION_FAILED", retryable=False):
        super().__init__(message)
        self.code = code
        self.retryable = retryable


@dataclass
class RunHandle:
    client: "SandboxClient"
    process: subprocess.Popen
    state_root: Path
    run_id: str

    def inspect(self):
        return self.client._command("inspect", self.run_id, state_root=self.state_root)

    def cancel(self):
        return self.client._command("cancel", self.run_id, state_root=self.state_root)

    def wait(self, timeout=None):
        # TimeoutExpired leaves execution running; call cancel explicitly to stop it.
        out, err = self.process.communicate(timeout=timeout)
        try:
            return json.loads(out)
        except (ValueError, TypeError) as error:
            raise SandboxError(err.decode(errors="replace")) from error

    def output_path(self, stream="stdout"):
        if stream not in ("stdout", "stderr"):
            raise ValueError("stream must be stdout/stderr")
        return self.state_root / self.run_id / (stream + ".bin")

class SandboxClient:
    def __init__(self, binary, state_root):
        self.binary = str(Path(binary).resolve())
        self.state_root = Path(state_root).resolve()
        self.state_root.mkdir(parents=True, exist_ok=True, mode=0o700)

    def _command(self, name, *args, state_root=None):
        p = subprocess.run([self.binary, name, *args, "--state-root", str(state_root or self.state_root)],
                           capture_output=True, timeout=5)
        if p.returncode:
            message = p.stderr.decode(errors="replace").strip()
            try:
                error = json.loads(message)
            except ValueError:
                error = None
            if isinstance(error, dict) and "code" in error:
                raise SandboxError(error.get("message", message), error["code"], error.get("retryable", False))
            raise SandboxError(message)
        return json.loads(p.stdout)

    def start(self, config_file, allow_unsafe=False, readiness_timeout=10, startup="auto"):
        # Each SDK request receives its own private root so discovering its run ID is unambiguous.
        root = Path(tempfile.mkdtemp(prefix="request-", dir=self.state_root))
        from .launch import plan_launch
        plan = plan_launch(self.binary, config_file, root, allow_unsafe=allow_unsafe, startup=startup)
        command = plan.argv
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + readiness_timeout
        while time.monotonic() < deadline:
            for state in root.glob("run-*/state.json"):
                return RunHandle(self, process, root, state.parent.name)
            if process.poll() is not None:
                out, err = process.communicate()
                try:
                    report = json.loads(out)
                except ValueError as error:
                    raise SandboxError(err.decode(errors="replace")) from error
                return RunHandle(self, process, root, report["run_id"])
            time.sleep(0.01)
        plan.stop(process)
        raise SandboxError("run ID readiness deadline exceeded")

    def run(self, config_file, allow_unsafe=False, timeout=None):
        return self.start(config_file, allow_unsafe=allow_unsafe).wait(timeout)
