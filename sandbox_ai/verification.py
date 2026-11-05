"""Explicit operator execution of immutable, named isolated templates. No shell."""
import json
from pathlib import Path
import re
import subprocess
import sys

from .evidence import read_file, MAX_REPORT
from .models import AnalysisError, digest


class Templates:
    def __init__(self, path):
        path = Path(path).resolve(strict=True)
        raw = json.loads(read_file(path.parent, path.name, MAX_REPORT, full=True)[0][0][1])
        if not isinstance(raw, dict) or len(raw) > 16:
            raise AnalysisError('Expected a map of at most 16 trusted templates')
        self.items = {}
        for name, spec in raw.items():
            if not re.fullmatch(r'[a-zA-Z0-9_-]{1,64}', name) or set(spec) != {'description', 'config'}:
                raise AnalysisError('Invalid template entry')
            config_path = (path.parent / spec['config']).resolve(strict=True)
            config = json.loads(read_file(config_path.parent, config_path.name, MAX_REPORT, full=True)[0][0][1])
            if config.get('mode') != 'isolated' or config.get('network') != 'none' or config.get('session', {}).get('enabled'):
                raise AnalysisError('Verification templates must be isolated, offline, single-run')
            limits = config.get('limits', {})
            for key, cap in [('wall_ms', 30000), ('startup_ms', 15000), ('grace_ms', 2000),
                             ('memory_bytes', 1024**3), ('pids', 128), ('capture_bytes', 1024**2),
                             ('tmp_bytes', 128*1024**2)]:
                if type(limits.get(key)) is not int or not 0 < limits[key] <= cap:
                    raise AnalysisError('Verification requires an explicit bounded limit: ' + key)
            for key in ('runtime_root', 'stdin_file', 'cgroup_root', 'workspace_transfer'):
                if config.get(key):
                    config[key] = str((config_path.parent / config[key]).resolve())
            for mount in config.get('mounts', []):
                mount['source'] = str((config_path.parent / mount['source']).resolve())
            self.items[name] = dict(id=name, description=spec['description'], config=config,
                                   digest=digest(config))

    def catalog(self):
        return [{k: item[k] for k in ('id', 'description', 'digest')} for item in self.items.values()]

    def get(self, name):
        if name not in self.items:
            raise AnalysisError('Unknown verification template')
        return self.items[name]


def execute(store, scope, analysis_id, proposal_id, templates, binary, output_dir):
    if sys.platform != 'linux':
        raise AnalysisError('Sandbox verification requires Linux; no process fallback')
    row = store.db.execute('SELECT body FROM analyses WHERE scope=? AND id=?', (scope, analysis_id)).fetchone()
    if not row:
        raise AnalysisError('Analysis unavailable in this project')
    analysis = json.loads(row[0])
    proposals = [p for p in analysis['proposals'] if p['id'] == proposal_id]
    if len(proposals) != 1:
        raise AnalysisError('Unknown proposal')
    proposal = proposals[0]
    template = templates.get(proposal['template_id'])
    if template['digest'] != proposal['template_digest']:
        raise AnalysisError('Template changed since proposal; generate a new proposal')
    root = Path(output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    config_path = root / 'config.json'
    config_path.write_text(json.dumps(template['config']), encoding='utf-8')
    state = root / 's'
    state.mkdir(mode=0o700)
    from sdk.launch import plan_launch
    plan = plan_launch(binary, config_path, state)
    process = subprocess.Popen(plan.argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    timeout = sum(template['config']['limits'][k] for k in ('startup_ms', 'wall_ms', 'grace_ms')) / 1000 + 30
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        plan.stop(process)
        raise
    (root / 'cli.stdout').write_bytes(stdout)
    (root / 'cli.stderr').write_bytes(stderr)
    try:
        report = json.loads(stdout)
        run_id = report['run_id']
        if not isinstance(run_id, str) or not re.fullmatch(r'run-[a-zA-Z0-9_-]+', run_id):
            raise ValueError()
    except (ValueError, KeyError, TypeError):
        raise AnalysisError('Verification returned no usable report; inspect CLI evidence') from None
    from .evidence import collect
    bundle = collect(scope, state / run_id, context='Verification of ' + analysis_id + ': ' + proposal['reason'])
    store.put_bundle(bundle)
    receipt = dict(schema_version=1, analysis_id=analysis_id, proposal_id=proposal_id,
                   template_digest=template['digest'], run_id=run_id, cli_exit=process.returncode,
                   execution=bundle.execution, status='executed_not_automatically_confirmed')
    (root / 'verification.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    return receipt
