"""Bounded, local batches. Each task owns an independent one-shot sandbox run."""
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import tempfile
import time
from .sandbox_client import SandboxClient, SandboxError
from .launch import plan_launch


@dataclass
class TaskHandle:
    batch: 'BatchHandle'
    task_id: str

    def status(self):
        return next(task for task in self.batch.status()['tasks'] if task['id'] == self.task_id)

    def cancel(self):
        return self.batch.cancel(self.task_id)

    def result(self):
        state = self.status()
        if not state['run_id']:
            return None
        path = Path(state['task_state_root']) / state['run_id'] / 'result.json'
        return json.loads(path.read_text()) if path.exists() else None

    def output_path(self, stream='stdout'):
        if stream not in ('stdout', 'stderr'):
            raise ValueError('invalid output stream')
        state = self.status()
        if not state['run_id']:
            raise SandboxError('task has not started', code='TASK_NOT_STARTED')
        return Path(state['task_state_root']) / state['run_id'] / (stream + '.bin')


@dataclass
class BatchHandle:
    client: 'BatchClient'
    state_root: Path
    run_id: str
    process: object = None

    def status(self):
        return json.loads((self.state_root/self.run_id/'batch.json').read_text())

    def task(self, task_id):
        if not any(item['id'] == task_id for item in self.status()['tasks']):
            raise ValueError('unknown task id')
        return TaskHandle(self, task_id)

    def cancel(self, task_id=None):
        if self.status()['batch_state'] == 'finished':
            return {'status': 'already_finished'}
        request = {'op': 'cancel'} if task_id is None else {'op': 'cancel-task', 'task_id': task_id}
        try:
            return self._control_request(request)
        except SandboxError:
            if self.status()['batch_state'] == 'finished':
                return {'status': 'already_finished'}
            raise

    def _control_request(self, request):
        # Batch control uses the existing JSON protocol independently of the
        # session transport; it shares the CLI entry point, not session logic.
        name = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', suffix='.json',
                                             dir=self.state_root, delete=False) as file:
                name = Path(file.name)
                json.dump({'protocol_version': 2, **request}, file)
            return self.client._command('session-request', self.run_id, '--request', str(name),
                                        state_root=self.state_root)
        finally:
            if name is not None:
                name.unlink(missing_ok=True)

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            state = self.status()
            if state['batch_state'] == 'finished':
                if self.process is not None:
                    remaining = None if deadline is None else max(0, deadline-time.monotonic())
                    self.process.wait(timeout=remaining)
                return state
            if self.process is not None and self.process.poll() is not None:
                raise SandboxError('batch supervisor exited before terminal report; inspect task state roots')
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError('batch wait expired; tasks remain running')
            time.sleep(0.02)


class BatchClient(SandboxClient):
    def start(self, config_file, allow_unsafe=False, readiness_timeout=20, startup='auto'):
        root = Path(tempfile.mkdtemp(prefix='batch-', dir=self.state_root))
        plan = plan_launch(self.binary, config_file, root, batch=True, allow_unsafe=allow_unsafe, startup=startup)
        with (root/'host.stdout').open('wb') as out, (root/'host.stderr').open('wb') as err:
            process = subprocess.Popen(plan.argv, stdout=out, stderr=err)
        deadline = time.monotonic()+readiness_timeout
        while time.monotonic() < deadline:
            for path in root.glob('run-*/batch.json'):
                return BatchHandle(self, root, path.parent.name, process)
            if process.poll() is not None:
                raise SandboxError((root/'host.stderr').read_text(errors='replace'))
            time.sleep(0.02)
        plan.stop(process)
        raise SandboxError('batch startup deadline exceeded')

    def attach(self, state_root, run_id):
        root = Path(state_root).resolve()
        self._command('inspect', run_id, state_root=root)
        handle = BatchHandle(self, root, run_id)
        handle.status()
        return handle
