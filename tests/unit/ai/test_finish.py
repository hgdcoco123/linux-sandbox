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
from sandbox_ai.service import validate_analysis
from sandbox_ai.store import Store
from sandbox_ai.verification import substitute


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


if __name__ == '__main__':
    unittest.main()
