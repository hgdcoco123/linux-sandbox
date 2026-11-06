"""The end-to-end drill's own logic, exercised with a fake sandbox.

The drill needs a delegated unit and the host policy relaxed, so its boundary branches
(failing target, hanging target, changed template) were only ever tested by running it for
real - which means a bug in the driver costs a round trip. This test replaces the sandbox
binary with a script that answers like the real one, so the branch logic and the assertions
are checked on every run of the suite.
"""
import json
import os
import sys
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

from sandbox_ai.evidence import collect
from sandbox_ai.models import AnalysisError
from sandbox_ai.store import Store
from sandbox_ai.verification import Templates

FAKE = '''#!/usr/bin/env python3
import json, sys
from pathlib import Path
args = sys.argv[1:]
config = json.loads(Path(args[args.index('--config') + 1]).read_text())
state = Path(args[args.index('--state-root') + 1])
mode = config.get('argv', ['x', 'success'])[1] if len(config.get('argv', [])) > 1 else 'success'
state.mkdir(parents=True, exist_ok=True)
run = state/('run-fake-' + mode)
run.mkdir(exist_ok=True)
if mode == 'ignore-term':
    report = dict(schema_version=2, run_id=run.name, summary='timeout', verdict='target_failed',
                  exit_code=None, signal=9, isolated=True, errors=[])
elif mode == 'session-read':
    report = dict(schema_version=2, run_id=run.name, summary='nonzero_exit', verdict='target_failed',
                  exit_code=43, signal=None, isolated=True, errors=[])
else:
    report = dict(schema_version=2, run_id=run.name, summary='success', verdict='ok',
                  exit_code=0, signal=None, isolated=True, errors=[])
(run/'result.json').write_text(json.dumps(report))
(run/'stdout.bin').write_bytes(b'')
(run/'stderr.bin').write_bytes(b'fake sandbox\\n')
print(json.dumps(report))
'''


@unittest.skipIf(os.name != 'posix', 'the fake sandbox is a shebang script')
class DrillTests(unittest.TestCase):
    def setUp(self):
        # The executor refuses non-Linux hosts; these tests replace the sandbox anyway.
        patcher = patch('sandbox_ai.verification.sys.platform', 'linux')
        self.addCleanup(patcher.stop)
        patcher.start()
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            'ai_verify_e2e', Path(__file__).resolve().parents[3]/'tools'/'ai_verify_e2e.py')
        self.e2e = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.e2e)
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sandbox = self.root/'fake-sandbox'
        self.sandbox.write_text(FAKE)
        self.sandbox.chmod(self.sandbox.stat().st_mode | stat.S_IXUSR)
        self.store = Store(self.root/'state.db')
        self.run = self.root/'run'
        self.run.mkdir()
        self.report = dict(schema_version=2, run_id='run-drill', summary='nonzero_exit',
                           verdict='target_failed', exit_code=43, isolated=True, errors=[])
        (self.run/'result.json').write_text(json.dumps(self.report))
        (self.run/'stdout.bin').write_bytes(b'')
        (self.run/'stderr.bin').write_bytes(b'undefined reference to calculate_total\n')
        self.store.put_bundle(collect('drill', self.run))
        self.args = type('Args', (), dict(
            sandbox=self.sandbox, runtime=self.root/'runtime', fixture='/usr/bin/sandbox-fixture'))()
        (self.root/'runtime').mkdir()

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def templates(self, name, argv, subdir='templates-drill', mount=None, **limits):
        folder = self.root/subdir
        folder.mkdir(exist_ok=True)
        config = dict(schema_version=1, mode='isolated', network='none',
                      runtime_root=str(self.root/'runtime'), cgroup_root=str(self.root/'cgroup'),
                      argv=argv, limits=dict(self.e2e.LIMITS, **limits))
        if mount:
            config['mounts'] = [mount]
        (folder/(name + '.json')).write_text(json.dumps(config))
        (folder/'templates.json').write_text(json.dumps({name: {'description': 'drill',
                                                                'config': name + '.json'}}))
        return Templates(folder/'templates.json')

    def test_the_template_sees_the_analysed_run_dir(self):
        # A verification that checks the input of the run that failed needs that run's files;
        # the placeholder is filled by the program, not by the model.
        registry = self.templates(
            'probe', ['x', 'session-write'], subdir='templates-run',
            mount={'source': '${run_dir}', 'target': '/evidence', 'readonly': True})
        receipt = self.e2e.propose_and_execute(self.store, 'drill', 'run-drill', self.root,
                                               self.args, registry, 'probe', 'with-run-dir')
        written = json.loads((self.root/'with-run-dir'/'config.json').read_text())
        self.assertEqual(written['mounts'][0]['source'], str(self.root/'run'))
        self.assertEqual(receipt['status'], 'executed_not_automatically_confirmed')

    def test_a_failing_target_is_recorded_as_target_failed(self):
        registry = self.templates('fail', ['x', 'session-read'])
        receipt = self.e2e.propose_and_execute(self.store, 'drill', 'run-drill', self.root,
                                               self.args, registry, 'fail', 'target-failed')
        self.assertEqual(receipt['status'], 'executed_not_automatically_confirmed')
        self.assertEqual(receipt['execution']['verdict'], 'target_failed')
        self.assertEqual(receipt['execution']['exit_code'], 43)

    def test_a_hanging_target_is_reported_as_timeout(self):
        registry = self.templates('hang', ['x', 'ignore-term'], wall_ms=200, grace_ms=200)
        receipt = self.e2e.propose_and_execute(self.store, 'drill', 'run-drill', self.root,
                                               self.args, registry, 'hang', 'target-timeout')
        self.assertEqual(receipt['execution']['summary'], 'timeout')
        self.assertEqual(receipt['execution']['signal'], 9)

    def test_a_successful_check_still_says_not_confirmed(self):
        registry = self.templates('ok', ['x', 'session-write'])
        receipt = self.e2e.propose_and_execute(self.store, 'drill', 'run-drill', self.root,
                                               self.args, registry, 'ok', 'ok')
        self.assertEqual(receipt['execution']['exit_code'], 0)
        self.assertEqual(receipt['status'], 'executed_not_automatically_confirmed')

    def test_a_changed_template_is_refused_before_anything_runs(self):
        registry = self.templates('patch', ['x', 'session-write'], subdir='templates-patch')
        from sandbox_ai.service import Analyzer
        model = self.e2e.Scripted([self.e2e.proposal_reply('patch', 'drill'),
                                   self.e2e.analysis_reply])
        analysis = Analyzer(self.store, model, templates=registry).analyze(
            'drill', 'run-drill', 'drill question')
        self.assertEqual(analysis['status'], 'completed')
        config = json.loads((self.root/'templates-patch'/'patch.json').read_text())
        config['limits']['wall_ms'] = 4000
        (self.root/'templates-patch'/'patch.json').write_text(json.dumps(config))
        reloaded = Templates(self.root/'templates-patch'/'templates.json')
        with self.assertRaisesRegex(AnalysisError, 'changed'):
            self.e2e.execute(self.store, 'drill', analysis['analysis_id'],
                             analysis['proposals'][0]['id'], reloaded,
                             self.sandbox, self.root/'changed')
        self.assertFalse((self.root/'changed').exists())


if __name__ == '__main__':
    unittest.main()
