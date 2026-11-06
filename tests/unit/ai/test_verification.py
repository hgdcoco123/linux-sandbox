"""Test the real executor contract with a fake launcher, not fake Linux assurance."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from sandbox_ai.models import AnalysisError
from sandbox_ai.store import Store
from sandbox_ai.verification import Templates, execute


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = Store(self.root/'state.db')
        config = dict(schema_version=1, mode='isolated', network='none', runtime_root='/runtime',
                      argv=['/usr/bin/check'],
                      limits=dict(wall_ms=1000, startup_ms=5000, grace_ms=100,
                                  memory_bytes=268435456, pids=32, capture_bytes=4096, tmp_bytes=33554432))
        (self.root/'config.json').write_text(json.dumps(config))
        (self.root/'templates.json').write_text(json.dumps({'check': {'description': 'test', 'config': 'config.json'}}))
        self.templates = Templates(self.root/'templates.json')
        self.analysis = dict(analysis_id='analysis-test', proposals=[dict(id='proposal-test', template_id='check',
            reason='verify a hypothesis', template_digest=self.templates.get('check')['digest'])])
        self.store.save_analysis('p', 'test', self.analysis)
        self.stopped = False
        self.launch = types.ModuleType('sdk.launch')
        self.launch.plan_launch = self.plan

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def plan(self, binary, config, state):
        self.assertEqual(json.loads(config.read_text())['mode'], 'isolated')
        self.state = state
        return types.SimpleNamespace(argv=['fake-sandbox', 'run'], stop=self.stop)

    def stop(self, process):
        self.stopped = True

    def invoke(self):
        return execute(self.store, 'p', 'analysis-test', 'proposal-test', self.templates,
                       '/trusted/sandbox', self.root/'results')

    def test_executes_template_imports_evidence_without_claiming_fix(self):
        owner = self
        class FakeProcess:
            returncode = 10
            def communicate(self, timeout):
                report = dict(run_id='run-verification', summary='nonzero_exit', exit_code=1, isolated=True)
                folder = owner.state/'run-verification'
                folder.mkdir()
                (folder/'result.json').write_text(json.dumps(report))
                (folder/'stdout.bin').write_bytes(b'')
                (folder/'stderr.bin').write_bytes(b'still fails')
                return json.dumps(report).encode(), b''
        with patch('sandbox_ai.verification.sys.platform', 'linux'), patch.dict(sys.modules, {'sdk.launch': self.launch}), \
                patch('sandbox_ai.verification.subprocess.Popen', return_value=FakeProcess()) as popen:
            receipt = self.invoke()
        self.assertEqual(receipt['status'], 'executed_not_automatically_confirmed')
        self.assertEqual(self.store.bundle('p', 'run-verification').execution['summary'], 'nonzero_exit')
        self.assertNotIn('shell', popen.call_args.kwargs)

    def test_watchdog_stops_launch_plan_and_leaves_a_receipt(self):
        class FakeProcess:
            def communicate(self, timeout):
                raise subprocess.TimeoutExpired('fake', timeout)
        with patch('sandbox_ai.verification.sys.platform', 'linux'), patch.dict(sys.modules, {'sdk.launch': self.launch}), \
                patch('sandbox_ai.verification.subprocess.Popen', return_value=FakeProcess()):
            with self.assertRaisesRegex(AnalysisError, 'timed out'):
                self.invoke()
        self.assertTrue(self.stopped)
        receipt = json.loads((self.root/'results'/'verification.json').read_text())
        self.assertEqual(receipt['status'], 'timed_out')
        self.assertIsNone(receipt['run_id'])

    def test_verification_is_recorded_for_the_conversation(self):
        owner = self
        class FakeProcess:
            returncode = 0
            def communicate(self, timeout):
                report = dict(run_id='run-verification', summary='success', exit_code=0, isolated=True)
                folder = Path(owner.state)/'run-verification'
                folder.mkdir()
                (folder/'result.json').write_text(json.dumps(report))
                (folder/'stdout.bin').write_bytes(b'libssl.so.3 looks fine\n')
                (folder/'stderr.bin').write_bytes(b'')
                return json.dumps(report).encode(), b''
        with patch('sandbox_ai.verification.sys.platform', 'linux'), patch.dict(sys.modules, {'sdk.launch': self.launch}), \
                patch('sandbox_ai.verification.subprocess.Popen', return_value=FakeProcess()):
            receipt = self.invoke()
        recorded = self.store.verifications('p', ['analysis-test'])
        self.assertEqual([item['run_id'] for item in recorded], ['run-verification'])
        self.assertEqual(recorded[0]['status'], receipt['status'])

    def test_child_environment_drops_credentials(self):
        from sandbox_ai.verification import child_environment
        with patch.dict('os.environ', {'SANDBOX_AI_API_KEY': 'secret', 'AWS_SECRET_ACCESS_KEY': 'secret',
                                       'XDG_RUNTIME_DIR': '/run/user/1000', 'PATH': '/usr/bin'}):
            env = child_environment()
        self.assertNotIn('SANDBOX_AI_API_KEY', env)
        self.assertNotIn('AWS_SECRET_ACCESS_KEY', env)
        self.assertEqual(env['XDG_RUNTIME_DIR'], '/run/user/1000')
        self.assertEqual(env['PATH'], '/usr/bin')

    def test_config_the_sandbox_would_reject_is_refused_up_front(self):
        broken = json.loads((self.root/'config.json').read_text())
        del broken['schema_version']
        (self.root/'config.json').write_text(json.dumps(broken))
        with self.assertRaisesRegex(AnalysisError, 'schema_version'):
            Templates(self.root/'templates.json')

    def test_changed_template_cannot_execute(self):
        self.templates.items['check']['digest'] = 'different'
        with patch('sandbox_ai.verification.sys.platform', 'linux'):
            with self.assertRaisesRegex(AnalysisError, 'changed'):
                self.invoke()
        self.assertFalse((self.root/'results').exists())

    def test_non_linux_does_not_fall_back(self):
        with patch('sandbox_ai.verification.sys.platform', 'win32'):
            with self.assertRaisesRegex(AnalysisError, 'Linux'):
                self.invoke()


if __name__ == '__main__':
    unittest.main()
