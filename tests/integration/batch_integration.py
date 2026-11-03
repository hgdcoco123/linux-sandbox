"""Independent task scheduling, cancellation and output regression on Linux."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sdk.batch_client import BatchClient

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='sbx-b-') as temp:
    root = Path(temp)
    client = BatchClient(binary, root/'s')
    execution_roots = []
    handles = []

    def config(argv):
        return {'schema_version': 1, 'mode': 'process', 'network': 'host', 'argv': argv,
                'cwd': str(root), 'limits': {'memory_bytes': 32*1024*1024, 'pids': 16,
                'wall_ms': 10000, 'grace_ms': 50, 'capture_bytes': 65536}}

    def start(tasks, parallel=2, memory=64*1024*1024, pids=32, unsafe=True):
        path = root/'batch.json'
        path.write_text(json.dumps({'schema_version': 1, 'max_parallel': parallel,
            'memory_budget_bytes': memory, 'pids_budget': pids,
            'tasks': [{'id': name, 'config': cfg} for name, cfg in tasks]}))
        handle = client.start(path, allow_unsafe=unsafe)
        execution_roots.append(Path(handle.status()['task_state_root']))
        handles.append(handle)
        return handle

    try:
        tasks = [('t'+str(i), config(['/bin/sh', '-c', 'sleep 0.2; head -c 1048576 /dev/zero'])) for i in range(4)]
        batch = start(tasks)
        report = batch.wait(30)
        assert report['summary'] == 'success' and report['peak_active'] == 2, report
        assert report['peak_reserved_memory_bytes'] <= 64*1024*1024 and report['peak_reserved_pids'] <= 32
        for i in range(4):
            result = batch.task('t'+str(i)).result()
            assert result['stdout']['saved_bytes'] == 65536 and result['stdout']['discarded_bytes'] == 1048576-65536, result
            assert result['stdout']['flushed'] and result['stdout']['queue_peak_bytes'] <= 256*1024
            assert batch.task('t'+str(i)).output_path().stat().st_size == 65536
        serial = start(tasks[:2], memory=32*1024*1024).wait(20)
        assert serial['peak_active'] == 1
        batch = start([('active', config(['/bin/sleep', '5'])), ('queued', config(['/bin/true']))], parallel=1)
        deadline = time.monotonic()+5
        while batch.task('active').status()['state'] != 'running':
            assert time.monotonic() < deadline
            time.sleep(0.01)
        batch.task('queued').cancel()
        batch.task('active').cancel()
        report = batch.wait(20)
        assert batch.task('queued').status()['state'] == 'cancelled', report
        assert batch.task('queued').status()['run_id'] is None
        assert batch.task('active').status()['state'] == 'cancelled', report
        mixed = start([('bad', config(['/does-not-exist'])), ('good', config(['/bin/true']))])
        mixed.wait(20)
        assert mixed.task('bad').status()['state'] == 'failed'
        assert mixed.task('good').status()['state'] == 'completed'
        print('batch scheduling, budget, cancellation and bounded output regression passed')

        runtime = os.environ.get('SANDBOX_TEST_RUNTIME')
        cgroup = os.environ.get('SANDBOX_TEST_CGROUP')
        if runtime and cgroup:
            tasks = [('isolated'+str(i), {'schema_version': 1,
                'argv': ['/usr/bin/sandbox-fixture', 'proc-view'],
                'runtime_root': runtime, 'cgroup_root': cgroup}) for i in range(4)]
            report = start(tasks, memory=512*1024*1024, pids=128, unsafe=False).wait(30)
            assert report['summary'] == 'success' and report['peak_active'] <= 2, report
            print('isolated batch smoke passed')
    finally:
        # Test-created, already terminal execution roots only. Preserve failed/live
        # roots for diagnosis rather than deleting their control files.
        for path, handle in zip(execution_roots, handles):
            if handle.status()['batch_state'] != 'finished' or handle.process.poll() is None:
                continue
            if path.parent == Path('/tmp') and path.name.startswith('sbx-tasks-'):
                runs = list(path.glob('*/run-*'))
                if runs and all((run/'result.json').exists() for run in runs):
                    shutil.rmtree(path)
