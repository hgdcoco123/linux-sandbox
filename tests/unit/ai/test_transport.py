import io
import json
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from sandbox_ai.models import AnalysisError
from sandbox_ai.providers import CompatibleProvider, NoRedirect


class TransportTests(unittest.TestCase):
    def provider(self):
        with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
            return CompatibleProvider('https://example.test/v1', 'chat-test', 'embed-test', key_env='TEST_AI_KEY')

    def test_request_and_native_tool_message(self):
        client = self.provider()
        message = {'role': 'assistant', 'content': None, 'tool_calls': [
            {'id': 'x', 'type': 'function', 'function': {'name': 'search_history', 'arguments': '{"query":"x"}'}}]}
        body = json.dumps({'choices': [{'message': message}], 'usage': {'prompt_tokens': 5}}).encode()
        with patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open.return_value = io.BytesIO(body)
            result = client.complete([{'role': 'user', 'content': 'why?'}], [{'type': 'function'}])
            req = factory.return_value.open.call_args.args[0]
            self.assertEqual(req.full_url, 'https://example.test/v1/chat/completions')
            self.assertEqual(req.get_header('Authorization'), 'Bearer fake-test-key')
            self.assertEqual(json.loads(req.data)['model'], 'chat-test')
            self.assertEqual(result['tool_calls'][0]['function']['name'], 'search_history')
            self.assertEqual(client.usage, [{'prompt_tokens': 5}])

    def test_embedding_normalization(self):
        client = self.provider()
        with patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open.return_value = io.BytesIO(b'{"data":[{"embedding":[3,4]}]}')
            self.assertEqual(client.embed('text'), [0.6, 0.8])
            req = factory.return_value.open.call_args.args[0]
            self.assertTrue(req.full_url.endswith('/embeddings'))

    def test_http_error_does_not_leak_response_or_key(self):
        client = self.provider()
        with patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open.side_effect = HTTPError('https://example.test', 401, 'fake-test-key', {}, None)
            with self.assertRaises(AnalysisError) as caught:
                client.complete([], [])
            self.assertNotIn('fake-test-key', str(caught.exception))
            self.assertIn('401', str(caught.exception))

    def test_plain_http_and_redirect_refused(self):
        with self.assertRaises(AnalysisError):
            CompatibleProvider('http://example.test/v1', 'model')
        with self.assertRaises(AnalysisError):
            NoRedirect().redirect_request(None, None, 302, '', {}, 'https://other.test')

    def test_non_json_and_oversize_response(self):
        client = self.provider()
        for body in (b'not json', b'x'*(2*1024*1024+1), b'[]'):
            with self.subTest(size=len(body)), patch('sandbox_ai.providers.build_opener') as factory:
                factory.return_value.open.return_value = io.BytesIO(body)
                with self.assertRaises(AnalysisError):
                    client.complete([], [])

    def test_rerank_request_and_ordering(self):
        with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
            client = CompatibleProvider('https://example.test/v1', 'chat-test', key_env='TEST_AI_KEY',
                                        rerank_model='rerank-test')
        self.assertTrue(client.rerank_identity.startswith('rerank:'))
        body = json.dumps({'results': [{'index': 0, 'relevance_score': 0.1},
                                       {'index': 1, 'relevance_score': 0.9}]}).encode()
        with patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open.return_value = io.BytesIO(body)
            ranked = client.rerank('why?', ['first', 'second'])
            request = json.loads(factory.return_value.open.call_args.args[0].data)
        self.assertEqual([index for index, _ in ranked], [1, 0])
        self.assertEqual(request['model'], 'rerank-test')
        self.assertEqual(request['documents'], ['first', 'second'])
        self.assertEqual(request['top_n'], 2)

    def test_rerank_refuses_a_partial_or_malformed_ranking(self):
        with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
            client = CompatibleProvider('https://example.test/v1', 'chat-test', key_env='TEST_AI_KEY',
                                        rerank_model='rerank-test')
        for body in (b'{"results":[{"index":0,"relevance_score":0.5}]}',
                     b'{"results":[{"index":5,"relevance_score":0.5},{"index":0,"relevance_score":0.1}]}',
                     b'{"results":[{"index":0,"relevance_score":"high"},{"index":1,"relevance_score":0.1}]}',
                     b'{"results":[{"index":0,"relevance_score":0.5},{"index":0,"relevance_score":0.1}]}'):
            with self.subTest(body=body), patch('sandbox_ai.providers.build_opener') as factory:
                factory.return_value.open.return_value = io.BytesIO(body)
                with self.assertRaises(AnalysisError):
                    client.rerank('why?', ['first', 'second'])
        with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
            without = CompatibleProvider('https://example.test/v1', 'chat-test', key_env='TEST_AI_KEY')
        with self.assertRaisesRegex(AnalysisError, 'rerank model'):
            without.rerank('why?', ['x'])
        # A thinking model can take minutes and a many-citation answer needs room; both are
        # client decisions, so they are bounded parameters rather than silent constants.
        client = self.provider()
        self.assertEqual(client.max_tokens, 3000)
        body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'x'}}]}).encode()
        with patch('sandbox_ai.providers.build_opener') as factory:
            factory.return_value.open.return_value = io.BytesIO(body)
            client.complete([{'role': 'user', 'content': 'q'}], [])
            self.assertEqual(json.loads(factory.return_value.open.call_args.args[0].data)['max_tokens'], 3000)
        for bad in (199, 8001, True, '3000'):
            with self.subTest(max_tokens=bad):
                with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
                    with self.assertRaises(AnalysisError):
                        CompatibleProvider('https://example.test/v1', 'chat-test',
                                           key_env='TEST_AI_KEY', max_tokens=bad)
        with patch.dict('os.environ', {'TEST_AI_KEY': 'fake-test-key'}):
            with self.assertRaises(AnalysisError):
                CompatibleProvider('https://example.test/v1', 'chat-test', key_env='TEST_AI_KEY',
                                   timeout=181)


if __name__ == '__main__':
    unittest.main()
