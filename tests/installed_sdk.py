"""Explicit installed SDK smoke; run as an ordinary Linux user with systemd user service."""
import json
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdk.session_client import SessionClient
from sdk.batch_client import BatchClient

if len(sys.argv) != 3:
    raise SystemExit('usage: installed_sdk.py /opt/linux-sandbox/bin/sandbox RUNTIME')
with tempfile.TemporaryDirectory(prefix='sbx-sdk-') as temp:
    root = Path(temp)
    config = root/'session.json'
    config.write_text(json.dumps({'schema_version': 1,
        'argv': ['/usr/bin/sandbox-fixture', 'echo'],
        'runtime_root': str(Path(sys.argv[2]).resolve()),
        'session': {'total_ms': 30000, 'idle_ms': 10000}}))
    # Deliberately omit cgroup_root and session.enabled: shared startup must delegate
    # and select persistent mode, without modifying the caller's configuration.
    client = SessionClient(sys.argv[1], root/'state')
    with client.create(config, readiness_timeout=20) as session:
        command = session.execute(['/usr/bin/sandbox-fixture', 'proc-view'], request_id='sdk-smoke')
        assert command.wait(5)['exit_code'] == 0
        assert session.execute(['/usr/bin/sandbox-fixture', 'proc-view'], request_id='sdk-smoke').command_id == command.command_id
    assert session.request_status('sdk-smoke')['state'] == 'finished'
    batch_config = root/'batch.json'
    batch_config.write_text(json.dumps({'schema_version': 1, 'max_parallel': 2,
        'memory_budget_bytes': 512*1024*1024, 'pids_budget': 128,
        'tasks': [{'id': 'task'+str(i), 'config': {'schema_version': 1,
            'argv': ['/usr/bin/sandbox-fixture', 'proc-view'],
            'runtime_root': str(Path(sys.argv[2]).resolve())}} for i in range(3)]}))
    batch = BatchClient(sys.argv[1], root/'b').start(batch_config)
    assert batch.wait(30)['summary'] == 'success'
    print('installed batch SDK automatic delegation passed; task records:', batch.status()['task_state_root'])
print('installed SDK automatic delegation and persistent readiness passed')
