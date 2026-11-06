"""python -m sandbox_ai --help"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from .evidence import collect
from .models import AnalysisError
from .providers import CompatibleProvider, DemoProvider
from .service import Analyzer
from .store import Store
from .verification import Templates, execute


def _progress(message):
    """A heartbeat for slow models: one line per five seconds, on a terminal only."""
    if sys.stderr.isatty():
        print('… ' + message, file=sys.stderr, flush=True)


def provider(args):
    if args.provider == 'demo':
        if args.semantic or getattr(args, 'rerank', False):
            raise AnalysisError('Demo does not simulate embeddings or a reranker; use a real provider')
        return DemoProvider()
    progress = None if getattr(args, 'quiet', False) else _progress
    return CompatibleProvider(args.base_url, args.model, args.embedding_model,
                              key_env=args.key_env, allow_local_http=args.allow_local_http,
                              rerank_model=os.environ.get('SANDBOX_AI_RERANK_MODEL', '')
                              if getattr(args, 'rerank', False) else '',
                              rerank_path=os.environ.get('SANDBOX_AI_RERANK_PATH', '/rerank'),
                              progress=progress)


def parser():
    p = argparse.ArgumentParser(description='Generic evidence-grounded sandbox analysis (local single-user CLI)')
    p.add_argument('--db', type=Path, default=Path.home()/'.sandbox-ai'/'state.sqlite3')
    p.add_argument('--scope', default='default', help='Project boundary; not a replacement for authentication')
    p.add_argument('--provider', choices=['demo', 'compatible'], default='compatible')
    p.add_argument('--base-url', default=os.environ.get('SANDBOX_AI_BASE_URL', ''))
    p.add_argument('--model', default=os.environ.get('SANDBOX_AI_MODEL', ''))
    p.add_argument('--embedding-model', default=os.environ.get('SANDBOX_AI_EMBEDDING_MODEL', ''))
    p.add_argument('--key-env', default='SANDBOX_AI_API_KEY')
    p.add_argument('--quiet', action='store_true', help='No progress heartbeat on stderr')
    p.add_argument('--allow-local-http', action='store_true')
    p.add_argument('--semantic', action='store_true', help='Use real embedding vectors with lexical retrieval')
    p.add_argument('--rerank', action='store_true',
                   help='Second stage: rerank the fused candidates with SANDBOX_AI_RERANK_MODEL')
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
    sub.add_parser('stats', help='Aggregate the recorded analyses of this scope')
    exp = sub.add_parser('explain', help='One question about one run: what happened, what is known')
    exp.add_argument('--run-dir', type=Path, required=True, help='Completed run directory')
    exp.add_argument('--question', default='这次运行为什么失败？请区分已确认事实、原因假设和仍缺少的信息。')
    exp.add_argument('--context', default='')
    exp.add_argument('--json', dest='as_json', action='store_true',
                     help='Print the machine-readable result instead of the human report')
    return p


def _failure_kind(error):
    """Bucket a failure by cause, so the aggregate says where the limit is."""
    text = (error or '').lower()
    for name, needles in (('transport', ('http', 'transport', 'timed out', 'deadline')),
                          ('citation', ('citation', 'line')),
                          ('schema', ('analysis fields', 'analysis list', 'invalid claim',
                                      'invalid next step', 'json')),
                          ('budget', ('budget', 'exhausted', 'oversized'))):
        if any(needle in text for needle in needles):
            return name
    return 'other' if text else 'unknown'


def stats(store, scope):
    """Pass rates over what was actually recorded, not over what the docs claim."""
    runs = store.analyses(scope)
    completed = [run for run in runs if run.get('status') == 'completed']
    metrics = [run.get('metrics') or {} for run in runs]
    def total(key):
        return sum(m.get(key) or 0 for m in metrics)
    def rate(count, size):
        return round(count / size, 3) if size else None
    failures = {}
    for run in runs:
        if run.get('status') != 'completed':
            kind = _failure_kind(run.get('error'))
            failures[kind] = failures.get(kind, 0) + 1
    return dict(scope=scope, analyses=len(runs), completed=len(completed),
                structured_output_rate=rate(len(completed), len(runs)),
                failure_reasons=failures,
                repairs=total('repairs'), citations=total('citations'),
                tool_calls=total('tool_calls'), tool_denied=total('tool_denied'),
                reranked=sum(1 for m in metrics if m.get('reranked')),
                semantic_degraded=sum(1 for m in metrics if m.get('semantic_degraded')),
                cache_hits=sum(1 for m in metrics if m.get('cache_hit')),
                retrieval_modes=sorted({m.get('retrieval') for m in metrics if m.get('retrieval')}),
                average_citations=round(total('citations') / len(completed), 2) if completed else None,
                verifications=len(store.db.execute(
                    'SELECT 1 FROM verifications WHERE scope=?', (scope,)).fetchall()),
                hypotheses={status: count for status, count in store.db.execute(
                    'SELECT status, COUNT(*) FROM hypothesis_status WHERE scope=? GROUP BY status',
                    (scope,)).fetchall()})


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


def explain(store, args, model):
    """One command, one call, no conversation: what happened, what is known, what is not.

    This is the shape the layer is meant to have for a user who just ran something and does
    not want to open a chat: it imports the run if needed, asks one question, and prints the
    answer with the resolved evidence line behind every claim.
    """
    bundle = collect(args.scope, args.run_dir, context=args.context)
    store.put_bundle(bundle)
    engine = Analyzer(store, model, semantic=args.semantic,
                      reranker=model if args.rerank else None)
    result = engine.analyze(args.scope, bundle.run_id, args.question)
    if getattr(args, 'as_json', False):
        print(json.dumps(dict(run_id=bundle.run_id, status=result['status'],
                              error=result.get('error'), execution=result['execution'],
                              analysis=result.get('analysis'),
                              resolved_citations=result.get('resolved_citations'),
                              metrics=result.get('metrics'), warnings=result.get('warnings')),
                         ensure_ascii=False, indent=2))
        return 2 if result['status'] != 'completed' else 0
    execution = result['execution'] or {}
    print('run: ' + bundle.run_id)
    print('execution: ' + str(execution.get('summary')) + ', verdict ' +
          str(execution.get('verdict')) + ', exit ' + str(execution.get('exit_code')))
    print('status: ' + result['status'] + ('  (' + str(result.get('error')) + ')'
                                           if result.get('error') else ''))
    analysis = result.get('analysis') or {}
    if analysis:
        print('\n' + str(analysis.get('summary')))
        resolved = {(item['id'], item['line']): item['text']
                    for item in result.get('resolved_citations') or []}
        for label, key in (('已确认', 'facts'), ('假设', 'hypotheses')):
            for claim in analysis.get(key) or []:
                print('\n[' + label + '] ' + str(claim.get('statement')))
                for ref in claim.get('citations') or []:
                    text = resolved.get((ref.get('id'), ref.get('line')), '')
                    print('    ' + str(ref.get('id')) + ' L' + str(ref.get('line')) + ': ' + text)
        for item in analysis.get('missing_info') or []:
            print('\n[未知] ' + str(item))
        for step in analysis.get('next_steps') or []:
            print('\n[建议] ' + str(step.get('action')) + '  → 期望: ' +
                  str(step.get('expected_observation')))
    for warning in result.get('warnings') or []:
        print('\n[warn] ' + str(warning))
    metrics = result.get('metrics') or {}
    print('\nretrieval: ' + str(metrics.get('retrieval')) + ' · citations ' +
          str(metrics.get('citations')) + ' · tools ' + str(metrics.get('tool_calls')) +
          ' · repairs ' + str(metrics.get('repairs')) + ' · ' +
          str(result.get('elapsed_seconds')) + 's')
    return 2 if result['status'] != 'completed' else 0


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
                              reranker=provider(args) if args.rerank else None,
                              templates=Templates(args.templates) if args.templates else None)
            result = engine.analyze(args.scope, args.run_id, args.question, args.session, args.refresh)
        elif args.command == 'explain':
            return explain(store, args, provider(args))
        elif args.command == 'verify':
            result = execute(store, args.scope, args.analysis_id, args.proposal_id,
                             Templates(args.templates), args.binary, args.results)
        elif args.command == 'stats':
            result = stats(store, args.scope)
        else:
            result = demo(store, args.scope, args.output)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 2 if isinstance(result, dict) and result.get('status') == 'failed' else 0
    except (AnalysisError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(json.dumps({'status': 'error', 'error': str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2
    finally:
        if store:
            store.close()


if __name__ == '__main__':
    raise SystemExit(main())
