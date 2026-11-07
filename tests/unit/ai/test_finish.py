"""The three finishing pieces: hypothesis status, substitution, retry.

None of these need a model or a sandbox: they are contract logic and transport behaviour.
"""
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from sandbox_ai.models import AnalysisError
from sandbox_ai.providers import CompatibleProvider
from sandbox_ai.service import Analyzer, validate_analysis
from sandbox_ai.store import Store
from sandbox_ai.evidence import chunks, line_reference, spans, prefix_lines, strip_line_prefix
from sandbox_ai.verification import Templates, substitute


def analysis(updates):
    return dict(summary='s', facts=[], hypotheses=[], missing_info=[], next_steps=[],
                hypothesis_updates=updates)


def seen():
    return {'ev-now': dict(id='ev-now', text='line from this run\n', origin='current'),
            'ev-ver': dict(id='ev-ver', text='line from the verification run\n', origin='verification')}


class HypothesisUpdates(unittest.TestCase):
    def test_supported_needs_a_verification_citation(self):
        validate_analysis(analysis([dict(statement='H', status='supported',
                                         citations=[{'id': 'ev-ver', 'line': 1}])]), seen())
        with self.assertRaisesRegex(AnalysisError, 'verification run'):
            validate_analysis(analysis([dict(statement='H', status='supported',
                                             citations=[{'id': 'ev-now', 'line': 1}])]), seen())

    def test_supported_without_any_citation_is_rejected(self):
        with self.assertRaisesRegex(AnalysisError, 'needs a citation'):
            validate_analysis(analysis([dict(statement='H', status='supported', citations=[])]), seen())

    def test_unknown_may_stand_alone(self):
        validate_analysis(analysis([dict(statement='H', status='unknown', citations=[])]), seen())

    def test_bad_status_is_rejected(self):
        with self.assertRaisesRegex(AnalysisError, 'supported, refuted or unknown'):
            validate_analysis(analysis([dict(statement='H', status='proven', citations=[])]), seen())

    def test_field_is_optional(self):
        value = dict(summary='s', facts=[], hypotheses=[], missing_info=[], next_steps=[])
        validate_analysis(value, seen())

    def test_updates_are_recorded_and_visible_to_a_later_turn(self):
        with tempfile.TemporaryDirectory() as temp:
            store = Store(Path(temp)/'s.db')
            try:
                store.save_hypothesis_updates('p', 'analysis-1', [
                    dict(statement='H1', status='supported', run_id='run-v1', citations=[]),
                    dict(statement='H2', status='unknown', run_id='', citations=[])])
                history = store.hypothesis_history('p')
                self.assertEqual([(item['statement'], item['status']) for item in history],
                                 [('H1', 'supported'), ('H2', 'unknown')])
                self.assertEqual(history[0]['run_id'], 'run-v1')
            finally:
                store.close()


class Substitution(unittest.TestCase):
    def test_run_dir_is_filled_everywhere(self):
        config = dict(argv=['/bin/check', '${run_dir}/artifacts'],
                      mounts=[{'source': '${run_dir}', 'target': '/evidence', 'readonly': True}],
                      runtime_root='/runtime')
        filled = substitute(config, '/state/run-1')
        self.assertEqual(filled['argv'][1], '/state/run-1/artifacts')
        self.assertEqual(filled['mounts'][0]['source'], '/state/run-1')
        self.assertEqual(filled['runtime_root'], '/runtime')

    def test_a_placeholder_without_a_run_dir_is_an_error(self):
        with self.assertRaisesRegex(AnalysisError, 'run directory'):
            substitute({'argv': ['${run_dir}']}, '')

    def test_plain_configs_are_untouched(self):
        config = {'argv': ['/bin/true'], 'mounts': []}
        self.assertEqual(substitute(config, '/x'), config)


class Retry(unittest.TestCase):
    def provider(self, retries=2):
        with patch.dict('os.environ', {'TEST_AI_KEY': 'k'}):
            return CompatibleProvider('https://example.test/v1', 'chat-test', key_env='TEST_AI_KEY',
                                      retries=retries)

    def body(self):
        return json.dumps({'choices': [{'message': {'role': 'assistant', 'content': '{}'}}]}).encode()

    def test_a_transient_error_is_retried(self):
        client = self.provider()
        attempts = []

        def open_once(request, timeout):
            attempts.append(1)
            if len(attempts) == 1:
                raise HTTPError('https://example.test', 429, 'busy', {}, None)
            return io.BytesIO(self.body())

        with patch('sandbox_ai.providers.time.sleep'), \
                patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open = open_once
            self.assertEqual(client.complete([{'role': 'user', 'content': 'q'}], []),
                             {'role': 'assistant', 'content': '{}'})
        self.assertEqual(len(attempts), 2)

    def test_a_bad_request_is_not_retried(self):
        client = self.provider()
        attempts = []

        def open_once(request, timeout):
            attempts.append(1)
            raise HTTPError('https://example.test', 400, 'bad', {}, None)

        with patch('sandbox_ai.providers.time.sleep'), \
                patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open = open_once
            with self.assertRaisesRegex(AnalysisError, 'HTTP 400'):
                client.complete([], [])
        self.assertEqual(len(attempts), 1)

    def test_retries_stop_at_the_budget(self):
        client = self.provider(retries=1)
        attempts = []

        def open_once(request, timeout):
            attempts.append(1)
            raise HTTPError('https://example.test', 503, 'down', {}, None)

        with patch('sandbox_ai.providers.time.sleep'), \
                patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open = open_once
            with self.assertRaisesRegex(AnalysisError, 'HTTP 503'):
                client.complete([], [])
        self.assertEqual(len(attempts), 2)


class ChunkAlignment(unittest.TestCase):
    def test_no_line_belongs_to_two_chunks(self):
        text = '\n'.join('line %03d payload' % index for index in range(400))
        pieces = list(spans(text))
        self.assertEqual(''.join(text[start:end] for start, end in pieces), text)
        self.assertTrue(all(end - start <= 1800 for start, end in pieces))
        seen = {}
        for start, end in pieces:
            for line in text[start:end].splitlines():
                seen[line] = seen.get(line, 0) + 1
        self.assertEqual(max(seen.values()), 1, 'a line appears in two chunks')

    def test_a_single_long_line_is_split_rather_than_dropped(self):
        text = 'x' * 5000
        pieces = list(spans(text))
        self.assertEqual(''.join(text[start:end] for start, end in pieces), text)
        self.assertTrue(all(end - start <= 1800 for start, end in pieces))

    def test_line_labels_round_trip(self):
        lines = prefix_lines('alpha\nbeta')
        self.assertEqual(lines, ['L1| alpha', 'L2| beta'])
        self.assertEqual(strip_line_prefix(lines[1]), 'beta')


class ImportKeepsIndex(unittest.TestCase):
    def bundle(self, root, marker):
        from sandbox_ai.evidence import collect
        run = root/'run'
        run.mkdir(exist_ok=True)
        (run/'result.json').write_text(json.dumps(dict(run_id='run-x', summary=marker, errors=[])))
        (run/'stdout.bin').write_bytes(b'')
        (run/'stderr.bin').write_bytes((marker + '\n').encode())
        return collect('p', run)

    def test_unchanged_snapshot_does_not_drop_vectors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            store = Store(root/'s.db')
            try:
                bundle = self.bundle(root, 'first')
                self.assertTrue(store.put_bundle(bundle))
                with store.db:
                    store.db.execute('INSERT INTO vectors VALUES(?,?,?)',
                                     (bundle.evidence[0].id, 'embedding:x', '[1]'))
                self.assertFalse(store.put_bundle(bundle), 'an identical snapshot must be skipped')
                kept = store.db.execute('SELECT COUNT(*) FROM vectors').fetchone()[0]
                self.assertEqual(kept, 1, 'a read-only re-import dropped the vector index')
                (root/'run'/'stderr.bin').write_bytes(b'changed\n')
                self.assertTrue(store.put_bundle(self.bundle(root, 'changed')))
                gone = store.db.execute('SELECT COUNT(*) FROM vectors').fetchone()[0]
                self.assertEqual(gone, 0, 'a changed snapshot must invalidate vectors')
            finally:
                store.close()

    def test_corpus_version_moves_when_evidence_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            store = Store(root/'s.db')
            try:
                before = store.corpus_version('p')
                store.put_bundle(self.bundle(root, 'first'))
                self.assertNotEqual(store.corpus_version('p'), before)
            finally:
                store.close()


class SelfCheck(unittest.TestCase):
    def analysis(self):
        return dict(summary='s',
                    facts=[dict(statement='the target exited 43',
                                citations=[dict(id='ev-1', line=1)])],
                    hypotheses=[], missing_info=[], next_steps=[])

    def provider(self, verdicts):
        import copy
        class Provider:
            identity = 'self-check'
            usage = []
            def complete(self, messages, tools, force_tool=False, force_tool_name=None):
                return dict(role='assistant', content=json.dumps(dict(verdicts=verdicts)))
        return Provider()

    def test_verdicts_are_recorded_not_enforced(self):
        import copy
        seen = {'ev-1': dict(id='ev-1', text='exit_code: 43\n', origin='current')}
        model = self.provider(['no'])
        analyzer = Analyzer(store=None, provider=model, self_check=True)
        result = analyzer.judge(self.analysis(), seen)
        self.assertEqual(result['checked'], 1)
        self.assertEqual(result['supported'], 0)
        self.assertEqual(result['unsupported'], 1)
        self.assertEqual(result['flagged'][0]['id'], 'ev-1')
        model_bad = self.provider(['maybe'])
        bad = Analyzer(store=None, provider=model_bad, self_check=True).judge(self.analysis(), seen)
        self.assertIn('error', bad)
        self.assertEqual(bad['checked'], 1)


class DatabaseContract(unittest.TestCase):
    def test_a_newer_database_is_refused(self):
        import sqlite3
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'future.db'
            db = sqlite3.connect(path)
            db.execute('PRAGMA user_version = 99')
            db.commit()
            db.close()
            with self.assertRaisesRegex(AnalysisError, 'newer version'):
                Store(path)

    def test_one_writer_at_a_time(self):
        import os
        if os.name != 'posix':
            self.skipTest('the advisory lock is POSIX only')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'s.db'
            first = Store(path)
            try:
                with self.assertRaisesRegex(AnalysisError, 'already open'):
                    Store(path)
            finally:
                first.close()
            second = Store(path)
            second.close()

    def test_a_fresh_database_records_its_schema_version(self):
        with tempfile.TemporaryDirectory() as temp:
            store = Store(Path(temp)/'s.db')
            try:
                version = store.db.execute('PRAGMA user_version').fetchone()[0]
                self.assertEqual(version, 1)
            finally:
                store.close()


class LegacyDatabase(unittest.TestCase):
    def test_a_database_with_duplicate_hypotheses_still_opens(self):
        # The unique index arrived after the table did; an older file must not become
        # unreadable just because it holds duplicates.
        import sqlite3
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'old.db'
            db = sqlite3.connect(path)
            db.executescript('''
              CREATE TABLE hypothesis_status(scope TEXT, statement TEXT, status TEXT,
                run_id TEXT, analysis_id TEXT, body TEXT);
            ''')
            db.executemany('INSERT INTO hypothesis_status VALUES(?,?,?,?,?,?)',
                           [('p', 'H', 'supported', 'run-1', 'a', '{}'),
                            ('p', 'H', 'supported', 'run-1', 'b', '{}')])
            db.commit()
            db.close()
            store = Store(path)
            try:
                rows = store.db.execute('SELECT COUNT(*) FROM hypothesis_status').fetchone()[0]
                self.assertEqual(rows, 1, 'duplicates were not collapsed before the index')
                history = store.hypothesis_history('p')
                self.assertEqual(len(history), 1)
            finally:
                store.close()


class Accounting(unittest.TestCase):
    def test_self_check_tokens_and_seconds_are_counted(self):
        class Provider:
            identity = 'accounting'
            def __init__(self):
                self.usage = []
            def complete(self, messages, tools, force_tool=False, force_tool_name=None):
                self.usage.append({'total_tokens': 10})
                if 'judge' in (messages[0].get('content') or ''):
                    return dict(role='assistant', content=json.dumps({'verdicts': ['yes']}))
                item = json.loads(messages[1]['content'])['evidence'][0]
                return dict(role='assistant', content=json.dumps(dict(
                    summary='s', facts=[dict(statement='the target exited 7',
                                             citations=[line_reference(item)])],
                    hypotheses=[], missing_info=[], next_steps=[])))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run = root/'run'
            run.mkdir()
            (run/'result.json').write_text(json.dumps(dict(run_id='run-a', summary='nonzero_exit', errors=[])))
            (run/'stdout.bin').write_bytes(b'')
            (run/'stderr.bin').write_bytes(b'exit code 7\n')
            from sandbox_ai.evidence import collect
            store = Store(root/'s.db')
            try:
                bundle = collect('p', run)
                store.put_bundle(bundle)
                provider = Provider()
                result = Analyzer(store, provider, self_check=True).analyze(
                    'p', bundle.run_id, 'why?')
                self.assertEqual(result['status'], 'completed')
                # two calls: the answer and the judge; both must appear in the report
                self.assertEqual(len(result['usage']), 2)
                self.assertEqual(result['metrics']['self_check']['supported'], 1)
                self.assertEqual(result['metrics']['self_check']['judge'], 'model')
            finally:
                store.close()


class CacheDiscipline(unittest.TestCase):
    def provider(self):
        class Provider:
            identity = 'cache-discipline'
            def __init__(self):
                self.usage = []
                self.calls = 0
            def complete(self, messages, tools, force_tool=False, force_tool_name=None):
                self.calls += 1
                self.usage.append({'total_tokens': 5})
                if 'judge' in (messages[0].get('content') or ''):
                    return dict(role='assistant', content=json.dumps({'verdicts': ['yes']}))
                item = json.loads(messages[1]['content'])['evidence'][0]
                return dict(role='assistant', content=json.dumps(dict(
                    summary='s', facts=[dict(statement='the target exited 7',
                                             citations=[line_reference(item)])],
                    hypotheses=[], missing_info=[], next_steps=[])))
        return Provider()

    def run_once(self, store, provider, bundle, **flags):
        return Analyzer(store, provider, **flags).analyze('p', bundle.run_id, 'why?')

    def test_asking_for_the_self_check_is_not_answered_from_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run = root/'run'
            run.mkdir()
            (run/'result.json').write_text(json.dumps(dict(run_id='run-a', summary='nonzero_exit', errors=[])))
            (run/'stdout.bin').write_bytes(b'')
            (run/'stderr.bin').write_bytes(b'exit code 7\n')
            from sandbox_ai.evidence import collect
            store = Store(root/'s.db')
            try:
                bundle = collect('p', run)
                store.put_bundle(bundle)
                provider = self.provider()
                first = self.run_once(store, provider, bundle)
                self.assertNotIn('self_check', first['metrics'])
                calls_before = provider.calls
                second = self.run_once(store, provider, bundle, self_check=True)
                self.assertGreater(provider.calls, calls_before,
                                   'the self-check request was answered from cache')
                self.assertIn('self_check', second['metrics'])
                self.assertFalse(second['metrics']['cache_hit'])
                self.assertEqual(second['metrics']['usage_records'], 2)
            finally:
                store.close()


class ProposalGuard(unittest.TestCase):
    def test_requiring_a_proposal_without_templates_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run = root/'run'
            run.mkdir()
            (run/'result.json').write_text(json.dumps(dict(run_id='run-a', summary='nonzero_exit', errors=[])))
            (run/'stdout.bin').write_bytes(b'')
            (run/'stderr.bin').write_bytes(b'')
            from sandbox_ai.evidence import collect
            store = Store(root/'s.db')
            try:
                bundle = collect('p', run)
                store.put_bundle(bundle)
                engine = Analyzer(store, object(), require_proposal=True)
                with self.assertRaisesRegex(AnalysisError, 'needs a template registry'):
                    engine.analyze('p', bundle.run_id, 'why?')
            finally:
                store.close()


class ReceiptWindow(unittest.TestCase):
    def test_the_window_is_the_whole_session(self):
        with tempfile.TemporaryDirectory() as temp:
            store = Store(Path(temp)/'s.db')
            try:
                session = store.session('p', None)
                for index in range(4):
                    store.remember('p', session, 'q' + str(index),
                                   dict(analysis_id='a' + str(index), run_id='run-' + str(index),
                                        status='completed', analysis=dict(summary='s')))
                store.save_verification('p', dict(
                    scope='p', analysis_id='a0', proposal_id='prop', run_id='run-verify',
                    status='executed_not_automatically_confirmed', template_digest='d',
                    execution=dict(summary='ok')))
                recent = store.verifications('p', ['a1', 'a2', 'a3'])
                whole = store.verifications_by_session('p', session)
                self.assertEqual(recent, [], 'the receipt is older than the three-turn window')
                self.assertEqual([item['run_id'] for item in whole], ['run-verify'])
            finally:
                store.close()


class TemplatesGuard(unittest.TestCase):
    def test_unknown_placeholder_is_refused_at_registration(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'cfg.json').write_text(json.dumps(dict(
                schema_version=1, mode='isolated', network='none',
                argv=['/bin/echo', '${artifact}'],
                limits=dict(wall_ms=1000, startup_ms=1000, grace_ms=100,
                            memory_bytes=1024, pids=4, capture_bytes=1024, tmp_bytes=1024))))
            (root/'templates.json').write_text(json.dumps(
                {'probe': {'description': 'd', 'config': 'cfg.json'}}))
            with self.assertRaisesRegex(AnalysisError, 'Unknown template placeholder'):
                Templates(root/'templates.json')


if __name__ == '__main__':
    unittest.main()
