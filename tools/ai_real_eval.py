"""Real-provider evaluation: the model path and citation fidelity, not a benchmark.

Imports already-finished run directories, asks the same question for each, and prints what
can be checked mechanically:

- did the answer pass the schema and every citation validation,
- were citations real quotes of supplied evidence (validation already enforces it),
- how many repair attempts, tool calls and denials happened,
- how much the provider actually reported as usage,
- did the model admit missing information instead of inventing a cause.

Three runs are a demonstration, not a measurement of diagnostic quality: the record says so.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sandbox_ai.evidence import collect
from sandbox_ai.providers import CompatibleProvider, DemoProvider
from sandbox_ai.service import Analyzer
from sandbox_ai.store import Store
from sandbox_ai.cli import stats

QUESTION = '这次运行为什么失败？请区分已确认事实、原因假设和仍缺少的信息。'


def make_provider(args):
    if args.provider == 'demo':
        return DemoProvider()
    return CompatibleProvider(args.base_url, args.model, args.embedding_model,
                              key_env=args.key_env, allow_local_http=args.allow_local_http,
                              timeout=args.timeout, max_tokens=args.max_tokens,
                              rerank_model=os.environ.get('SANDBOX_AI_RERANK_MODEL', '')
                              if args.rerank else '',
                              rerank_path=os.environ.get('SANDBOX_AI_RERANK_PATH', '/rerank'))


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--provider', choices=['demo', 'compatible'], default='compatible')
    parser.add_argument('--runs', nargs='+', required=True, help='finished run directories')
    parser.add_argument('--db', type=Path)
    parser.add_argument('--scope', default='eval')
    parser.add_argument('--question', default=QUESTION)
    parser.add_argument('--base-url', default=os.environ.get('SANDBOX_AI_BASE_URL', ''))
    parser.add_argument('--model', default=os.environ.get('SANDBOX_AI_MODEL', ''))
    parser.add_argument('--embedding-model', default=os.environ.get('SANDBOX_AI_EMBEDDING_MODEL', ''))
    parser.add_argument('--key-env', default='SANDBOX_AI_API_KEY')
    parser.add_argument('--allow-local-http', action='store_true')
    parser.add_argument('--timeout', type=int, default=90, help='per-request HTTP timeout, seconds')
    parser.add_argument('--max-tokens', type=int, default=3000, help='answer budget per request')
    parser.add_argument('--deadline', type=int, default=180, help='wall deadline for one analysis, seconds')
    parser.add_argument('--rerank', action='store_true', help='Second stage rerank of the fused candidates')
    parser.add_argument('--limit', type=int, default=0, help='Only run the first N cases (cost guard)')
    args = parser.parse_args(argv)

    work = args.db.parent if args.db else Path(tempfile.mkdtemp(prefix='sbx-ai-eval-'))
    work.mkdir(parents=True, exist_ok=True)
    store = Store(args.db or work/'eval.sqlite3')
    rows = []
    try:
        provider = make_provider(args)
        print('== provider: ' + getattr(provider, 'identity', args.provider), file=sys.stderr)
        runs = args.runs[:args.limit] if args.limit else args.runs
        # Rough cost preview: prompt characters are the dominant term, and a provider
        # bills per token; 4 characters per token is the usual rule of thumb.
        print('== cases: ' + str(len(runs)) + ', rough prompt estimate: ' +
              str(sum(len(Path(p, 'result.json').read_text()) for p in runs) // 4) +
              ' tokens before any tool call or repair', file=sys.stderr)
        for path in runs:
            bundle = collect(args.scope, Path(path),
                             context='评估用例：一条已经完成的真实运行。')
            store.put_bundle(bundle)
            started = time.monotonic()
            result = Analyzer(store, provider, deadline_seconds=args.deadline,
                              reranker=provider if args.rerank else None).analyze(
                args.scope, bundle.run_id, args.question)
            analysis = result.get('analysis') or {}
            rows.append(dict(
                run=bundle.run_id,
                execution=result['execution'].get('summary'),
                status=result['status'],
                error=result.get('error'),
                facts=len(analysis.get('facts') or []),
                hypotheses=len(analysis.get('hypotheses') or []),
                citations=result['metrics']['citations'],
                repairs=result['metrics']['repairs'],
                tools=result['metrics']['tool_calls'],
                denied=result['metrics']['tool_denied'],
                missing_info=len(analysis.get('missing_info') or []),
                context_chars=result['metrics']['context_chars'],
                usage=result['usage'],
                seconds=round(time.monotonic() - started, 2),
                summary=(analysis.get('summary') or '')[:160]))
        summary = stats(store, args.scope)
    finally:
        store.close()

    print(json.dumps(dict(cases=rows, aggregate=summary), ensure_ascii=False, indent=2))
    failed = [row for row in rows if row['status'] != 'completed']
    if failed:
        print('FAIL: ' + str(len(failed)) + ' analysis did not pass validation', file=sys.stderr)
        return 1
    print('== all analyses passed schema and citation validation; ' +
          str(len(rows)) + ' cases, ' + str(sum(row['citations'] for row in rows)) +
          ' citations, ' + str(sum(row['denied'] for row in rows)) + ' denials', file=sys.stderr)
    print('== this is a demonstration on ' + str(len(rows)) + ' runs, not a quality benchmark',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
