"""Explicit operator execution of immutable, named isolated templates. No shell."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from .evidence import read_file, MAX_REPORT
from .models import AnalysisError, digest


RUNDIR = '${run_dir}'
_PLACEHOLDER = re.compile(r'\$\{[^}]*\}')


def _reject_unknown_placeholders(value):
    """Only ${run_dir} is substituted; anything else would reach the sandbox verbatim."""
    if isinstance(value, str):
        for name in _PLACEHOLDER.findall(value):
            if name != RUNDIR:
                raise AnalysisError('Unknown template placeholder ' + name +
                                    ' (only ${run_dir} is supported)')
    elif isinstance(value, list):
        for item in value:
            _reject_unknown_placeholders(item)
    elif isinstance(value, dict):
        for item in value.values():
            _reject_unknown_placeholders(item)


def _resolve_template_path(value, base):
    """Resolve a config path, leaving `${run_dir}` for the executor to fill.

    Resolving a placeholder against the registry directory first would turn
    `templates/${run_dir}` into `templates//abs/path`; the placeholder has to survive
    registration untouched, and it is the only thing the executor substitutes.
    """
    if RUNDIR in value:
        return value
    return str((base / value).resolve())


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
            # A template the sandbox itself will reject is not a trusted template: catch it
            # when the registry is read, not after the operator has already executed it.
            _reject_unknown_placeholders(config)
            if config.get('schema_version') != 1:
                raise AnalysisError('Verification templates must declare schema_version 1')
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
                    config[key] = _resolve_template_path(config[key], config_path.parent)
            for mount in config.get('mounts', []):
                mount['source'] = _resolve_template_path(mount['source'], config_path.parent)
            self.items[name] = dict(id=name, description=spec['description'], config=config,
                                   digest=digest(config))

    def catalog(self):
        return [{k: item[k] for k in ('id', 'description', 'digest')} for item in self.items.values()]

    def get(self, name):
        if name not in self.items:
            raise AnalysisError('Unknown verification template')
        return self.items[name]


def child_environment():
    """The environment a verification run may inherit.

    systemd --user needs its session variables, so this is a filter rather than a
    whitelist of three names - but anything that looks like a credential stays here. The
    sandbox CLI does not need model credentials, and a service unit would otherwise carry
    them into the environment of every process it starts.
    """
    dropped = ('SANDBOX_AI_API_KEY',)
    pattern = re.compile(r'(?i)(password|passwd|secret|api[_-]?key|authorization|access[_-]?token)')
    return {name: value for name, value in os.environ.items()
            if name not in dropped and not pattern.search(name)}


def substitute(config, run_dir):
    """Fill `${run_dir}` with the analysed run's directory, everywhere in the template.

    A verification that checks the *input* of a failing run ("would this archive unpack
    again?") has to be able to point at that run's files. The placeholder is the only thing
    substituted, and its value is a path the program computed - never a path from the model -
    so the operator's registry stays the authority on what runs.
    """
    if isinstance(config, str):
        if '${run_dir}' in config:
            if not run_dir:
                raise AnalysisError('This template needs the run directory, but the snapshot '
                                    'did not record one; re-import the run')
            return config.replace('${run_dir}', run_dir)
        return config
    if isinstance(config, list):
        return [substitute(item, run_dir) for item in config]
    if isinstance(config, dict):
        return {key: substitute(value, run_dir) for key, value in config.items()}
    return config


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
    # Older analyses may predate run_id; a template without ${run_dir} still works for them.
    run_dir = store.bundle(scope, analysis['run_id']).run_dir if analysis.get('run_id') else ''
    config = substitute(template['config'], run_dir)
    root = Path(output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    config_path = root / 'config.json'
    config_path.write_text(json.dumps(config), encoding='utf-8')
    state = root / 's'
    state.mkdir(mode=0o700)
    from sdk.launch import plan_launch
    plan = plan_launch(binary, config_path, state)
    process = subprocess.Popen(plan.argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=child_environment())
    timeout = sum(template['config']['limits'][k] for k in ('startup_ms', 'wall_ms', 'grace_ms')) / 1000 + 30
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        # Leave the evidence on disk before reporting: a timed-out verification is exactly
        # the case where the operator wants to see what it was doing.
        plan.stop(process)
        (root / 'verification.json').write_text(json.dumps(dict(
            schema_version=1, analysis_id=analysis_id, proposal_id=proposal_id,
            template_digest=template['digest'], run_id=None, cli_exit=None,
            status='timed_out', timeout_seconds=timeout,
            note='output was not captured; the launch plan was stopped'),
            ensure_ascii=False, indent=2), encoding='utf-8')
        raise AnalysisError('Verification timed out; the launch plan was stopped') from None
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
        (root / 'verification.json').write_text(json.dumps(dict(
            schema_version=1, analysis_id=analysis_id, proposal_id=proposal_id,
            template_digest=template['digest'], run_id=None, cli_exit=process.returncode,
            status='no_report', note='inspect cli.stdout and cli.stderr'),
            ensure_ascii=False, indent=2), encoding='utf-8')
        raise AnalysisError('Verification returned no usable report; inspect CLI evidence') from None
    from .evidence import collect
    bundle = collect(scope, state / run_id, context='Verification of ' + analysis_id + ': ' + proposal['reason'])
    store.put_bundle(bundle)
    receipt = dict(schema_version=1, analysis_id=analysis_id, proposal_id=proposal_id,
                   template_digest=template['digest'], run_id=run_id, cli_exit=process.returncode,
                   hypothesis=proposal.get('hypothesis', ''),
                   execution=bundle.execution, status='executed_not_automatically_confirmed')
    (root / 'verification.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    # Recorded in the store as well, so the conversation that proposed it can see that the
    # run exists and what it was for. The receipt stays "executed, not confirmed": only a
    # human reading the new evidence can decide whether the hypothesis survived.
    store.save_verification(scope, dict(receipt, scope=scope))
    return receipt
