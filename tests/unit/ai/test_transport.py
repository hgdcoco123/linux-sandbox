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


if __name__ == '__main__':
    unittest.main()
