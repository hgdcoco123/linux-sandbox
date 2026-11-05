"""Bounded snapshots. Never execute parsers or follow links in untrusted artifacts.

Import completed, quiescent directories only. This is a local-user adapter, not a
filesystem security boundary against concurrent hostile directory replacement.
"""
import csv
import io
import json
import os
from pathlib import Path
import re
import stat

from .models import AnalysisError, Bundle, canonical, evidence

MAX_FILE = 64 * 1024
MAX_REPORT = 1024 * 1024
MAX_FILES = 32
MAX_EVIDENCE = 256
TEXT_TYPES = {'.txt', '.log', '.md', '.json', '.csv', '.tsv', '.py', '.c', '.cpp',
              '.h', '.hpp', '.rs', '.go', '.java', '.js', '.ts', '.yaml', '.yml',
              '.toml', '.ini', '.cfg', '.sh', '.xml', '.cmake'}
SECRET_KEYS = re.compile(r'(?i)(password|passwd|secret|api[_-]?key|authorization|access[_-]?token)')


def redact(text):
    text = re.sub(r'(?i)(bearer\s+)[A-Za-z0-9._~+/=-]+', r'\1[REDACTED]', text)
    return re.sub(r'(?i)((?:password|secret|api[_-]?key|access[_-]?token)\s*[=:]\s*)[^\s,;]+',
                  r'\1[REDACTED]', text)


def clean_json(value):
    if isinstance(value, dict):
        return {k: '[REDACTED]' if SECRET_KEYS.search(k) else clean_json(v) for k, v in value.items()}
    if isinstance(value, list):
        return [clean_json(v) for v in value]
    return redact(value) if isinstance(value, str) else value


def read_file(root, relative, limit=MAX_FILE, full=False):
    root = Path(root).resolve(strict=True)
    relative = Path(relative)
    if relative.is_absolute() or '..' in relative.parts:
        raise AnalysisError('Path must be relative and contained in the registered root')
    path = root
    for part in relative.parts:
        path = path / part
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise AnalysisError('Links/reparse points are not evidence: ' + str(relative))
    path.resolve(strict=True).relative_to(root)
    flags = os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_BINARY', 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise AnalysisError('Only regular, non-hardlinked files are accepted')
        if full and before.st_size > limit:
            raise AnalysisError('Structured file exceeds size limit')
        with os.fdopen(fd, 'rb', closefd=False) as file:
            if before.st_size <= limit:
                pieces = [(0, file.read(limit))]
            else:
                pieces = [(0, file.read(limit // 2))]
                file.seek(before.st_size - limit // 2)
                pieces.append((file.tell(), file.read(limit // 2)))
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise AnalysisError('Evidence changed during read; import only completed runs')
        return pieces, before.st_size
    finally:
        os.close(fd)


def chunks(scope, run_id, kind, source, text, location='text', truncated=False):
    text = redact(text)
    for start in range(0, len(text), 1600):
        end = min(start + 1800, len(text))
        yield evidence(scope, run_id, kind, source, f'{location}; chars {start}:{end}',
                       text[start:end], truncated)
        if end == len(text):
            break


def collect(scope, run_dir, artifact_root=None, source_root=None, context=None):
    if not scope or len(scope) > 128:
        raise AnalysisError('A nonempty project scope (<=128 characters) is required')
    run_dir = Path(run_dir).resolve(strict=True)
    pieces, _ = read_file(run_dir, 'result.json', MAX_REPORT, full=True)
    report = json.loads(pieces[0][1])
    # Both one-shot RunReport and persistent command core records are supported.
    if isinstance(report, dict) and 'result' in report:
        result = report['result']
        if not isinstance(result, dict) or not isinstance(result.get('outcome'), dict):
            raise AnalysisError('Invalid command result/outcome')
        outcome = result.get('outcome', {})
        run_id = run_dir.parent.name + '/' + str(report.get('index', run_dir.name))
        execution = dict(summary=outcome.get('kind', 'unknown'), outcome=outcome,
                         cleanup=result.get('cleanup'), timing=result.get('timing'),
                         stdout=result.get('stdout'), stderr=result.get('stderr'))
    elif isinstance(report, dict) and 'summary' in report and 'run_id' in report:
        run_id = str(report['run_id'])
        execution = {k: report.get(k) for k in ('summary', 'verdict', 'exit_code', 'signal',
                    'isolated', 'cleanup', 'timing', 'errors', 'stdout', 'stderr', 'workspace',
                    'stop_reason', 'exec_error', 'metrics', 'security', 'runtime_prepared')}
    else:
        raise AnalysisError('result.json is not a supported final execution report')
    if not run_id or len(run_id) > 256 or '\0' in run_id:
        raise AnalysisError('Invalid run identity')
    execution = clean_json(execution)
    items = [evidence(scope, run_id, 'report', 'result.json', 'execution', canonical(execution))]
    warnings = []
    for name in ('config.json', 'request.json'):
        try:
            pieces, _ = read_file(run_dir, name, MAX_REPORT, full=True)
            config = clean_json(json.loads(pieces[0][1]))
            items.extend(chunks(scope, run_id, 'config', name, canonical(config), 'normalized JSON'))
        except FileNotFoundError:
            pass
        except (AnalysisError, ValueError, OSError):
            warnings.append(name + ': unavailable or invalid; configuration not analyzed')
    for name in ('stdout.bin', 'stderr.bin'):
        try:
            pieces, size = read_file(run_dir, name)
        except FileNotFoundError:
            warnings.append(name + ' missing; not equivalent to empty output')
            continue
        for offset, data in pieces:
            items.extend(chunks(scope, run_id, 'log', name, data.decode('utf-8', errors='replace'),
                                f'bytes {offset}:{offset + len(data)}', size > MAX_FILE))
        if size > MAX_FILE:
            warnings.append(name + ': middle omitted; head and tail retained')
    for kind, root in (('artifact', artifact_root), ('source', source_root)):
        if root is None:
            continue
        root = Path(root).resolve(strict=True)
        visited = 0
        directories = 0
        for base, dirs, files in os.walk(root, followlinks=False):
            directories += 1
            if directories > 128:
                warnings.append(kind + ': directory scan capped at 128')
                break
            dirs[:] = sorted(d for d in dirs if not d.startswith('.') and
                             not (Path(base) / d).is_symlink())
            for name in sorted(files):
                if visited >= MAX_FILES:
                    break
                visited += 1
                relative = (Path(base) / name).relative_to(root)
                label = kind + '/' + relative.as_posix()
                try:
                    pieces, size = read_file(root, relative)
                    suffix = relative.suffix.lower()
                    items.append(evidence(scope, run_id, 'metadata', label, 'file',
                                          canonical(dict(path=label, size=size, suffix=suffix))))
                    if suffix not in TEXT_TYPES or any(b'\0' in p for _, p in pieces):
                        warnings.append(label + ': metadata only; unsupported content')
                        continue
                    text = pieces[0][1].decode('utf-8', errors='replace')
                    if suffix == '.json' and size <= MAX_FILE:
                        text = json.dumps(clean_json(json.loads(text)), ensure_ascii=False, indent=2)
                    elif suffix in ('.csv', '.tsv') and size <= MAX_FILE:
                        rows = list(csv.reader(io.StringIO(text), delimiter='\t' if suffix == '.tsv' else ','))
                        items.append(evidence(scope, run_id, 'check', label, 'csv structure', canonical({
                            'header': rows[0] if rows else [], 'data_rows': max(0, len(rows)-1),
                            'ragged_rows': sum(len(r) != len(rows[0]) for r in rows[1:]) if rows else 0})))
                    for offset, data in pieces:
                        body = text if len(pieces) == 1 else data.decode('utf-8', errors='replace')
                        items.extend(chunks(scope, run_id, kind, label, body,
                                            f'file bytes {offset}:{offset+len(data)}', size > MAX_FILE))
                except (OSError, ValueError, AnalysisError, csv.Error) as error:
                    warnings.append(label + ': extraction unavailable (' + type(error).__name__ + ')')
            if visited >= MAX_FILES:
                warnings.append(kind + ': file scan capped at ' + str(MAX_FILES))
                break
    if context:
        if not isinstance(context, str) or len(context) > 12000:
            raise AnalysisError('Caller context must be text of at most 12000 characters')
        # Context is supplied by the caller, never interpreted as execution facts.
        items.extend(chunks(scope, run_id, 'context', 'caller-context', clean_json(context)))
    if len(items) > MAX_EVIDENCE:
        warnings.append('Evidence capped; unselected material is not analyzed')
    # Bound large report text too, preserving the full factual execution object in the bundle.
    report_item = items.pop(0)
    reports = list(chunks(scope, run_id, 'report', report_item.source, report_item.text))
    return Bundle(scope, run_id, execution, (reports + items)[:MAX_EVIDENCE], warnings)
