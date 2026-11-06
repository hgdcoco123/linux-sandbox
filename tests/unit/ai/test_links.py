"""The parts added after the first review: lifecycle evidence, bounded re-reads,
origin labels and the verification link. No model, no sandbox."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from sandbox_ai.cli import demo, stats
from sandbox_ai.evidence import collect, quote_line, read_span, MAX_READ
from sandbox_ai.models import AnalysisError, canonical
from sandbox_ai.service import Analyzer, validate_analysis
from sandbox_ai.store import Store


def run_dir(root, name='run', report=None, stderr=b'undefined reference to calculate_total\n',
            internal=None):
    folder = Path(root)/name
    folder.mkdir(parents=True, exist_ok=True)
    document = report or dict(run_id='run-' + name, summary='nonzero_exit', exit_code=1,
                              verdict='target_failed', isolated=True, errors=[],
                              cleanup=dict(descendants_empty=True), stdout={}, stderr={})
    (folder/'result.json').write_text(json.dumps(document))
    (folder/'stdout.bin').write_bytes(b'')
    (folder/'stderr.bin').write_bytes(stderr)
    if internal is not None:
        (folder/'internal.log').write_text(internal)
    return folder


class Reply:
    identity = 'test-reply'
    def __init__(self, replies):
        self.replies = list(replies)
        self.usage = []
        self.calls = []

    def complete(self, messages, tools):
        self.calls.append(json.loads(messages[1]['content']))
        value = self.replies.pop(0)
        return value(messages) if callable(value) else value


def answer(messages, index=0):
    item = json.loads(messages[1]['content'])['evidence'][index]
    return dict(role='assistant', content=canonical(dict(summary='summary',
        facts=[dict(statement='observed', citations=[dict(id=item['id'],
                                                          quote=quote_line(item['text']))])],
        hypotheses=[], missing_info=[], next_steps=[])))


def call(name, arguments):
    return dict(role='assistant', content=None, tool_calls=[dict(id='call-1', type='function',
                function=dict(name=name, arguments=canonical(arguments)))])


class LifecycleEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = Store(self.root/'state.db')

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def test_lifecycle_log_is_imported(self):
        folder = run_dir(self.root, internal='[00:00:01.000] [info] close: reason=signal\n')
        bundle = collect('p', folder)
        sources = {(item.kind, item.source) for item in bundle.evidence}
        self.assertIn(('log', 'internal.log'), sources)
        text = ' '.join(item.text for item in bundle.evidence if item.source == 'internal.log')
        self.assertIn('reason=signal', text)

    def test_missing_lifecycle_log_is_not_a_warning(self):
        bundle = collect('p', run_dir(self.root))
        self.assertFalse([w for w in bundle.warnings if 'internal.log' in w])

    def test_range_reads_are_bounded_and_refuse_other_files(self):
        folder = run_dir(self.root, stderr=b'0123456789abcdef', internal='line\n')
        span = read_span(folder, 'stderr.bin', 4, 6)
        self.assertEqual(span['text'], '456789')
        self.assertTrue(span['truncated'])
        self.assertEqual(span['size'], 16)
        with self.assertRaisesRegex(AnalysisError, 'captured streams'):
            read_span(folder, 'result.json', 0, 10)
        with self.assertRaisesRegex(AnalysisError, 'out of bounds'):
            read_span(folder, 'stderr.bin', 0, MAX_READ + 1)
        with self.assertRaisesRegex(AnalysisError, 'out of bounds'):
            read_span(folder, 'stderr.bin', -1, 4)


class CitationRules(unittest.TestCase):
    def test_short_quote_is_rejected(self):
        item = dict(id='ev-1', text='undefined reference to calculate_total')
        value = dict(summary='s', facts=[dict(statement='observed',
                     citations=[dict(id='ev-1', quote='undef')])],
                     hypotheses=[], missing_info=[], next_steps=[])
        with self.assertRaisesRegex(AnalysisError, 'at least 8'):
            validate_analysis(value, {'ev-1': item})

    def test_reflowed_json_quote_within_one_line_is_accepted(self):
        # Models reflow compact JSON when they quote it; within one line only whitespace
        # may differ.
        text = '{\n  "summary": "oom",\n  "verdict": "harness_error"\n}'
        value = dict(summary='s', facts=[dict(statement='observed',
                     citations=[dict(id='ev-1', quote='"summary":"oom",')])],
                     hypotheses=[], missing_info=[], next_steps=[])
        validate_analysis(value, {'ev-1': dict(id='ev-1', text=text)})

    def test_paraphrased_quote_is_still_rejected(self):
        text = '{\n  "summary": "oom",\n  "verdict": "harness_error"\n}'
        value = dict(summary='s', facts=[dict(statement='observed',
                     citations=[dict(id='ev-1', quote='"summary": "out of memory"')])],
                     hypotheses=[], missing_info=[], next_steps=[])
        with self.assertRaisesRegex(AnalysisError, 'single line'):
            validate_analysis(value, {'ev-1': dict(id='ev-1', text=text)})

    def test_quote_spanning_two_real_lines_is_rejected(self):
        # The failure that motivated this rule: both halves exist, but not next to each other.
        text = '{\n  "exit_code": 0,\n  "verdict": "ok"\n}'
        value = dict(summary='s', facts=[dict(statement='observed',
                     citations=[dict(id='ev-1', quote='"exit_code": 0,\\n  "verdict": "ok"')])],
                     hypotheses=[], missing_info=[], next_steps=[])
        with self.assertRaisesRegex(AnalysisError, 'single line'):
            validate_analysis(value, {'ev-1': dict(id='ev-1', text=text)})

    def test_double_escaped_quote_within_one_line_is_accepted(self):
        # A model can escape a quote once too often inside its JSON; unescaping recovers
        # exactly the characters that are on that line.
        text = '{\n  "stdout": {"eof": true}\n}'
        value = dict(summary='s', facts=[dict(statement='observed',
                     citations=[dict(id='ev-1', quote='\\"stdout\\": {\\"eof\\": true}')])],
                     hypotheses=[], missing_info=[], next_steps=[])
        validate_analysis(value, {'ev-1': dict(id='ev-1', text=text)})

    def test_report_evidence_is_pretty_printed(self):
        # Compact JSON is hard to quote exactly and hard to read; the report is stored
        # indented so a model can copy a line instead of re-serialising it.
        with tempfile.TemporaryDirectory() as temp:
            folder = run_dir(Path(temp), report=dict(run_id='run-x', summary='oom', metrics=dict(oom=1)))
            bundle = collect('p', folder)
            report = [item for item in bundle.evidence if item.kind == 'report'][0]
            self.assertIn('\n', report.text)
            self.assertIn('"metrics": {', report.text)


class OriginsAndVerification(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = Store(self.root/'state.db')
        self.run = run_dir(self.root, 'run-current')
        self.other = run_dir(self.root, 'run-other', stderr=b'another failure with calculate_total\n')
        self.store.put_bundle(collect('p', self.run))
        self.store.put_bundle(collect('p', self.other))

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def test_evidence_carries_its_origin(self):
        provider = Reply([answer])
        result = Analyzer(self.store, provider).analyze('p', 'run-run-current', 'why?')
        origins = {item['origin'] for item in provider.calls[0]['evidence']}
        self.assertIn('current', origins)
        self.assertEqual(result['metrics']['origins'].get('verification'), 0)

    def test_verification_run_reaches_the_next_turn(self):
        self.store.put_bundle(collect('p', self.other))
        session = self.store.session('p')
        first = dict(analysis_id='analysis-first', run_id='run-run-current', status='completed',
                     analysis=dict(summary='first'))
        self.store.remember('p', session, 'why?', first)
        self.store.save_verification('p', dict(analysis_id='analysis-first', proposal_id='proposal-1',
                                               template_digest='d', run_id='run-run-other',
                                               status='executed_not_automatically_confirmed',
                                               execution=dict(summary='success')))
        provider = Reply([answer])
        result = Analyzer(self.store, provider).analyze('p', 'run-run-current', 'did it hold?',
                                                        session_id=session)
        prompt = provider.calls[0]
        self.assertEqual(prompt['verification_context'][0]['run_id'], 'run-run-other')
        self.assertEqual(prompt['verification_context'][0]['status'],
                         'executed_not_automatically_confirmed')
        self.assertIn('verification', {item['origin'] for item in prompt['evidence']})
        self.assertGreaterEqual(result['metrics']['origins']['verification'], 1)

    def test_read_output_tool_is_citable_and_recorded(self):
        def cite_span(messages):
            payload = json.loads([m for m in messages if m['role'] == 'tool'][-1]['content'])
            return dict(role='assistant', content=canonical(dict(summary='read',
                facts=[dict(statement='the log names the symbol',
                            citations=[dict(id=payload['id'], quote=quote_line(payload['text'], 20))])],
                hypotheses=[], missing_info=[], next_steps=[])))
        provider = Reply([call('read_output', dict(stream='stderr.bin', offset=0, length=20)), cite_span])
        result = Analyzer(self.store, provider).analyze('p', 'run-run-current', 'show the log')
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(result['metrics']['tool_calls'], 1)
        self.assertEqual(result['metrics']['citations'], 1)
        self.assertTrue([item for item in result['evidence'] if 'range 0:20' in item['location']])

    def test_read_output_denies_other_files(self):
        provider = Reply([call('read_output', dict(stream='result.json', offset=0, length=10)), answer])
        result = Analyzer(self.store, provider).analyze('p', 'run-run-current', 'peek')
        self.assertEqual(result['metrics']['tool_denied'], 1)
        self.assertEqual(result['metrics']['tool_calls'], 1)
        self.assertEqual(result['status'], 'completed')


class Aggregates(unittest.TestCase):
    def test_corpus_cache_is_dropped_when_a_bundle_is_added(self):
        # The corpus read used to happen on every query and every search_history call; the
        # cache must never serve a list that predates an import.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            store = Store(root/'state.db')
            try:
                store.put_bundle(collect('p', run_dir(root/'a', 'run-a', stderr=b'first failure\n')))
                first = [e.run_id for e in store.corpus('p')]
                self.assertIn('run-run-a', first)
                store.put_bundle(collect('p', run_dir(root/'b', 'run-b', stderr=b'second failure\n')))
                self.assertIn('run-run-b', [e.run_id for e in store.corpus('p')])
                self.assertTrue([hit for hit in store.search('p', 'second failure')
                                 if hit['evidence']['run_id'] == 'run-run-b'])
            finally:
                store.close()

    def test_stats_counts_what_was_recorded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            store = Store(root/'state.db')
            try:
                demo(store, 'demo', root/'out')
                summary = stats(store, 'demo')
            finally:
                store.close()
        self.assertEqual(summary['analyses'], 3)
        self.assertEqual(summary['completed'], 3)
        self.assertEqual(summary['structured_output_rate'], 1.0)
        self.assertEqual(summary['tool_denied'], 0)
        self.assertGreaterEqual(summary['tool_calls'], 1)


if __name__ == '__main__':
    unittest.main()
