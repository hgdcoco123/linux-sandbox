"""Launch routing contracts; subprocesses and host policy are not changed."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk.launch import plan_launch, delegation_command, LaunchPlan
from sdk.session_client import SessionHandle
from sdk.sandbox_client import SandboxClient, SandboxError


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        (self.root/'build').mkdir()
        (self.root/'tools').mkdir()
        (self.root/'tools/delegated_run.py').write_text('# test')
        self.binary = self.root/'build/sandbox'
        self.config = self.root/'config.json'

    def save(self, **value):
        self.config.write_text(json.dumps(value))

    def test_process_requires_explicit_consent(self):
        self.save(mode='process')
        with self.assertRaises(ValueError):
            plan_launch(self.binary, self.config, self.root)
        plan = plan_launch(self.binary, self.config, self.root, session=True, allow_unsafe=True)
        self.assertIn('session-open', plan.argv)
        self.assertIn('--allow-unsafe', plan.argv)
        self.assertIsNone(plan.unit)

    def test_existing_delegation_is_reused(self):
        self.save(cgroup_root='/sys/fs/cgroup/example/runs')
        plan = plan_launch(self.binary, self.config, self.root, session=True)
        self.assertEqual(plan.argv[1], 'session-open')
        self.assertIsNone(plan.unit)

    def test_sdk_and_wrapper_share_delegation_arguments(self):
        self.save()
        with patch('sdk.launch.os.geteuid', return_value=1000, create=True):
            plan = plan_launch(self.binary, self.config, self.root, session=True)
        expected = delegation_command(self.binary.resolve(), self.root/'tools/delegated_run.py',
                                      config=self.config, state_root=self.root, session=True, unit=plan.unit)
        self.assertEqual(plan.argv, expected)
        self.assertIn('--session', plan.argv)
        self.assertIn('Delegate=yes', plan.argv)
        self.assertNotIn('sudo', plan.argv)

    def test_timeout_stops_unit_before_reaping_wrapper(self):
        process = Mock()
        process.poll.return_value = None
        plan = LaunchPlan([], 'test.service')
        with patch('sdk.launch.subprocess.run') as run:
            run.return_value.returncode = 0
            plan.stop(process)
        self.assertEqual(run.call_args.args[0], ['/usr/bin/systemctl', '--user', 'stop', 'test.service'])
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=10)

    def test_execute_exposes_stable_retry_id(self):
        session = SessionHandle(None, self.root, 'unused')
        session.request = Mock(return_value={'command_id': 'cmd-1'})
        session.execute(['/bin/true'], request_id='task-step-1')
        self.assertEqual(session.request.call_args.kwargs['request_id'], 'task-step-1')
        session.execute(['/bin/true'])
        self.assertEqual(len(session.request.call_args.kwargs['request_id']), 32)
        session.request.side_effect = SandboxError('lost response')
        with self.assertRaises(SandboxError) as caught:
            session.execute(['/bin/true'], request_id='keep-me')
        self.assertEqual(caught.exception.request_id, 'keep-me')

    def test_cli_structured_errors_survive_sdk(self):
        client = SandboxClient(self.binary, self.root/'state')
        with patch('sdk.sandbox_client.subprocess.run') as run:
            run.return_value.returncode = 2
            run.return_value.stderr = b'{"code":"REQUEST_CONFLICT","message":"conflict","retryable":false}'
            with self.assertRaises(SandboxError) as caught:
                client._command('session-request')
        self.assertEqual(caught.exception.code, 'REQUEST_CONFLICT')
        self.assertFalse(caught.exception.retryable)

    def test_batch_start_routes_to_batch_command(self):
        self.save(tasks=[{'id': 'a', 'config': {'mode': 'process'}}])
        plan = plan_launch(self.binary, self.config, self.root, batch=True, allow_unsafe=True)
        self.assertEqual(plan.argv[1], 'batch')
        self.assertIn('--allow-unsafe', plan.argv)

    def test_isolated_batch_delegation_flag(self):
        self.save(tasks=[{'id': 'a', 'config': {'mode': 'isolated'}}])
        with patch('sdk.launch.os.geteuid', return_value=1000, create=True):
            plan = plan_launch(self.binary, self.config, self.root, batch=True)
        self.assertIn('--batch', plan.argv)
        self.assertNotIn('--session', plan.argv)


if __name__ == '__main__':
    unittest.main()
