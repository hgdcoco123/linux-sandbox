"""Verified-TLS chat/tools + embedding transport; no secrets in persisted state."""
import json
import os
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .models import AnalysisError, canonical, digest
from .store import normalize


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise AnalysisError('Model endpoint redirect refused')


class CompatibleProvider:
    def __init__(self, base_url, model, embedding_model='', key_env='SANDBOX_AI_API_KEY', timeout=60,
                 allow_local_http=False):
        parsed = urlsplit(base_url)
        local = parsed.hostname in ('localhost', '127.0.0.1', '::1')
        if (parsed.scheme != 'https' and not (allow_local_http and local and parsed.scheme == 'http')
                or not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment):
            raise AnalysisError('Use an HTTPS base URL, or explicitly allow loopback HTTP')
        if not model or not 0 < timeout <= 120:
            raise AnalysisError('Model and bounded timeout are required')
        self.base = base_url.rstrip('/')
        self.model = model
        self.embedding_model = embedding_model
        self.key = os.environ.get(key_env, '')
        if not self.key and not (local and allow_local_http):
            raise AnalysisError('Model credential environment variable is not set')
        self.timeout = timeout
        self.identity = 'compatible:' + digest([self.base, model])
        self.embedding_identity = 'embedding:' + digest([self.base, embedding_model])
        self.usage = []

    def post(self, endpoint, payload):
        request = Request(self.base + endpoint, data=canonical(payload).encode(),
                          headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + self.key})
        try:
            with build_opener(NoRedirect()).open(request, timeout=self.timeout) as response:
                body = response.read(2 * 1024 * 1024 + 1)
                if len(body) > 2 * 1024 * 1024:
                    raise AnalysisError('Model response exceeds byte budget')
                result = json.loads(body)
        except HTTPError as error:
            raise AnalysisError(f'Model HTTP {error.code}; response body omitted') from None
        except (URLError, TimeoutError, OSError, ValueError):
            raise AnalysisError('Model transport or JSON response failed') from None
        usage = result.get('usage', {}) if isinstance(result, dict) else {}
        if not isinstance(result, dict):
            raise AnalysisError('Model response must be an object')
        if isinstance(usage, dict):
            self.usage.append({k: v for k, v in usage.items() if type(v) in (int, float)})
        return result

    def complete(self, messages, tools):
        payload = dict(model=self.model, messages=messages, temperature=0, max_tokens=2400)
        if tools:
            payload.update(tools=tools, tool_choice='auto', parallel_tool_calls=False)
        response = self.post('/chat/completions', payload)
        try:
            choice = response['choices'][0]
            if choice.get('finish_reason') in ('length', 'content_filter'):
                raise AnalysisError('Model response was incomplete')
            message = choice['message']
            if message.get('role') != 'assistant':
                raise AnalysisError('Invalid model message role')
            return {k: message[k] for k in ('role', 'content', 'tool_calls') if k in message}
        except (KeyError, IndexError, TypeError, AttributeError):
            raise AnalysisError('Unexpected chat response format') from None

    def embed(self, text):
        if not self.embedding_model:
            raise AnalysisError('An embedding model is required for semantic retrieval')
        response = self.post('/embeddings', dict(model=self.embedding_model, input=text))
        try:
            return normalize(response['data'][0]['embedding'])
        except (KeyError, IndexError, TypeError):
            raise AnalysisError('Unexpected embedding response format') from None


class DemoProvider:
    """Deterministic plumbing demo, deliberately not an AI or diagnosis benchmark."""
    identity = 'offline-demo-v1'
    usage = ()

    def complete(self, messages, tools):
        initial = json.loads(messages[1]['content'])
        if tools and not any(m['role'] == 'tool' for m in messages):
            return dict(role='assistant', content=None, tool_calls=[dict(id='demo-search', type='function',
                function=dict(name='search_history', arguments=canonical({'query': initial['question']})))])
        item = initial['evidence'][0]
        analysis = dict(summary='离线演示：已读取执行证据；未调用真实模型，不推断故障原因。',
            facts=[dict(statement='执行报告包含以下记录。', citations=[dict(id=item['id'], quote=item['text'][:160])])],
            hypotheses=[], missing_info=['需配置真实模型后评估诊断质量。'],
            next_steps=[dict(action='检查报告和日志中的原始证据。', expected_observation='确认执行状态。')])
        return dict(role='assistant', content=canonical(analysis))
