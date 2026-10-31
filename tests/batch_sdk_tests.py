import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.batch_client import BatchHandle
from sdk.sandbox_client import SandboxError


class BatchSDKTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.directory = self.root/'run-test'
        self.directory.mkdir()
        self.batch = BatchHandle(None, self.root, 'run-test')
        self.save()

    def save(self, state='running'):
        (self.directory/'batch.json').write_text(json.dumps({'batch_state': state,
            'summary': 'success' if state == 'finished' else None,
            'tasks': [{'id': 'a', 'state': 'queued', 'run_id': None, 'task_state_root': str(self.root/'task')}]}))

    def test_queued_task_has_no_result_or_output(self):
        task = self.batch.task('a')
        self.assertIsNone(task.result())
        with self.assertRaises(SandboxError) as caught:
            task.output_path()
        self.assertEqual(caught.exception.code, 'TASK_NOT_STARTED')

    def test_unknown_task_is_rejected(self):
        with self.assertRaises(ValueError):
            self.batch.task('missing')

    def test_wait_timeout_leaves_tasks_running(self):
        with self.assertRaises(TimeoutError):
            self.batch.wait(timeout=0)
        self.assertEqual(self.batch.status()['batch_state'], 'running')

    def test_finished_batch_does_not_send_cancel(self):
        self.save('finished')
        with patch.object(self.batch, '_control_request') as handle:
            self.assertEqual(self.batch.cancel()['status'], 'already_finished')
            handle.assert_not_called()
        self.assertEqual(self.batch.wait(0)['summary'], 'success')

    def test_cancel_race_only_accepts_confirmed_terminal(self):
        with patch.object(self.batch, '_control_request') as handle:
            def ended(request):
                self.save('finished')
                raise SandboxError('socket closed')
            handle.side_effect = ended
            self.assertEqual(self.batch.cancel('a')['status'], 'already_finished')
            self.save()
            handle.side_effect = SandboxError('live failure')
            with self.assertRaises(SandboxError):
                self.batch.cancel('a')

    def test_cancel_uses_legacy_batch_request_and_removes_file(self):
        self.batch.client = Mock()
        def command(name, run_id, flag, filename, state_root):
            self.assertEqual((name, run_id, flag), ('session-request', 'run-test', '--request'))
            self.assertEqual(state_root, self.root)
            self.assertEqual(json.loads(Path(filename).read_text()),
                             {'protocol_version': 2, 'op': 'cancel-task', 'task_id': 'a'})
            return {'status': 'accepted'}
        self.batch.client._command.side_effect = command
        self.assertEqual(self.batch.cancel('a'), {'status': 'accepted'})
        self.assertEqual(list(self.root.glob('*.json')), [])


if __name__ == '__main__':
    unittest.main()
