"""python -m sandbox_ai --help"""
import argparse
import json
import os
from pathlib import Path
import sys

from .evidence import collect
from .models import AnalysisError
from .providers import CompatibleProvider, DemoProvider
from .service import Analyzer
from .store import Store
from .verification import Templates, execute


def provider(args):
    if args.provider == 'demo':
        if args.semantic:
            raise AnalysisError('Demo does not simulate semantic embeddings; use a real embedding provider')
        return DemoProvider()
    return CompatibleProvider(args.base_url, args.model, args.embedding_model,
                              key_env=args.key_env, allow_local_http=args.allow_local_http)


def parser():
    p = argparse.ArgumentParser(description='Generic evidence-grounded sandbox analysis (local single-user CLI)')
    p.add_argument('--db', type=Path, default=Path.home()/'.sandbox-ai'/'state.sqlite3')
    p.add_argument('--scope', default='default', help='Project boundary; not a replacement for authentication')
    p.add_argument('--provider', choices=['demo', 'compatible'], default='compatible')
    p.add_argument('--base-url', default=os.environ.get('SANDBOX_AI_BASE_URL', ''))
    p.add_argument('--model', default=os.environ.get('SANDBOX_AI_MODEL', ''))
    p.add_argument('--embedding-model', default=os.environ.get('SANDBOX_AI_EMBEDDING_MODEL', ''))
    p.add_argument('--key-env', default='SANDBOX_AI_API_KEY')
    p.add_argument('--allow-local-http', action='store_true')
    p.add_argument('--semantic', action='store_true', help='Use real embedding vectors with lexical retrieval')
    sub = p.add_subparsers(dest='command', required=True)
    imp = sub.add_parser('import', help='Snapshot a completed one-shot run or persistent command directory')
    imp.add_argument('--run-dir', type=Path, required=True)
    imp.add_argument('--artifacts', type=Path)
    imp.add_argument('--sources', type=Path)
    imp.add_argument('--context', default='')
    sub.add_parser('index', help='Build vectors when --semantic is specified; lexical indexing is automatic')
    search = sub.add_parser('search')
    search.add_argument('query')
    analyze = sub.add_parser('analyze')
    analyze.add_argument('--run-id', required=True)
    analyze.add_argument('--question', required=True)
    analyze.add_argument('--session', help='Continue a persisted analysis conversation')
    analyze.add_argument('--templates', type=Path, help='Trusted verification registry')
    analyze.add_argument('--refresh', action='store_true')
    verify = sub.add_parser('verify', help='Explicitly execute a previously proposed trusted template')
    verify.add_argument('--analysis-id', required=True)
    verify.add_argument('--proposal-id', required=True)
    verify.add_argument('--templates', type=Path, required=True)
    verify.add_argument('--binary', type=Path, required=True)
    verify.add_argument('--results', type=Path, required=True)
    demo = sub.add_parser('demo', help='Synthetic fixtures and deterministic model; no Linux or API calls')
    demo.add_argument('--output', type=Path, required=True, help='New directory')
    return p


def demo(store, scope, output):
    output.mkdir(parents=True, exist_ok=False)
    stream = dict(eof=True, flushed=True, saved_bytes=0, discarded_bytes=0)
    examples = [
        ('compile', 'nonzero_exit', 1, 'main.cpp:8: undefined reference to `calculate_total`\n',
         'C++ build: generate a command line program. Synthetic fixture, never executed.'),
        ('dataset', 'success', 0, '',
         'CSV requires name,value columns and 2 data rows. Synthetic fixture, never executed.'),
        ('timeout', 'timeout', None, '',
         'Program exceeded a wall deadline; root cause is unknown. Synthetic fixture, never executed.')]
    for name, summary, code, stderr, context in examples:
        folder = output / name
        folder.mkdir()
        run_id = 'demo-' + name
        report = dict(schema_version=2, run_id=run_id, summary=summary, exit_code=code,
                      verdict='ok' if code == 0 else 'target_failed', isolated=True,
                      stdout=stream, stderr=stream, errors=[],
                      cleanup=dict(descendants_empty=True, launcher_reaped=True, cgroup_removed=True, errors=[]))
        (folder/'result.json').write_text(json.dumps(report), encoding='utf-8')
        (folder/'stdout.bin').write_bytes(b'')
        (folder/'stderr.bin').write_bytes(stderr.encode())
        artifacts = None
        if name == 'dataset':
            artifacts = folder/'artifacts'
            artifacts.mkdir()
            (artifacts/'data.csv').write_text('name,value\na,10\nb\n', encoding='utf-8')
        bundle = collect(scope, folder, artifact_root=artifacts, context=context)
        store.put_bundle(bundle)
    engine = Analyzer(store, DemoProvider())
    first = engine.analyze(scope, 'demo-compile', '构建失败的证据是什么？')
    second = engine.analyze(scope, 'demo-dataset', '这份 CSV 有什么结构问题？', session_id=first['session_id'])
    third = engine.analyze(scope, 'demo-timeout', '超时是否就能证明死循环？', session_id=first['session_id'])
    for index, result in enumerate((first, second, third), 1):
        (output/f'analysis-{index}.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return dict(mode='synthetic_offline_demo_not_real_ai_or_isolation',
                statuses=[r['status'] for r in (first, second, third)], session_id=first['session_id'],
                search=store.search(scope, 'undefined reference'), output=str(output.resolve()))


def main(argv=None):
    args = parser().parse_args(argv)
    store = None
    try:
        store = Store(args.db)
        if args.command == 'import':
            bundle = collect(args.scope, args.run_dir, args.artifacts, args.sources, args.context)
            store.put_bundle(bundle)
            result = dict(run_id=bundle.run_id, evidence_count=len(bundle.evidence), warnings=bundle.warnings)
        elif args.command == 'index':
            count = store.embed(args.scope, provider(args)) if args.semantic else len(store.corpus(args.scope))
            result = dict(chunks=count, mode='semantic' if args.semantic else 'lexical')
        elif args.command == 'search':
            result = store.search(args.scope, args.query, provider=provider(args) if args.semantic else None)
        elif args.command == 'analyze':
            engine = Analyzer(store, provider(args), semantic=args.semantic,
                              templates=Templates(args.templates) if args.templates else None)
            result = engine.analyze(args.scope, args.run_id, args.question, args.session, args.refresh)
        elif args.command == 'verify':
            result = execute(store, args.scope, args.analysis_id, args.proposal_id,
                             Templates(args.templates), args.binary, args.results)
        else:
            result = demo(store, args.scope, args.output)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 2 if isinstance(result, dict) and result.get('status') == 'failed' else 0
    except (AnalysisError, OSError, ValueError, KeyError) as error:
        print(json.dumps({'status': 'error', 'error': str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    finally:
        if store:
            store.close()


if __name__ == '__main__':
    raise SystemExit(main())
