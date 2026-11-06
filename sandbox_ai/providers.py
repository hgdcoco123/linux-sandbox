"""Verified-TLS chat/tools + embedding transport; no secrets in persisted state."""
import json
import os
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .evidence import line_reference
from .models import AnalysisError, canonical, digest
from .store import normalize


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise AnalysisError('Model endpoint redirect refused')


class CompatibleProvider:
    def __init__(self, base_url, model, embedding_model='', key_env='SANDBOX_AI_API_KEY', timeout=90,
                 allow_local_http=False, max_tokens=3000, rerank_model='',
                 rerank_path='/rerank'):
        parsed = urlsplit(base_url)
        local = parsed.hostname in ('localhost', '127.0.0.1', '::1')
        if (parsed.scheme != 'https' and not (allow_local_http and local and parsed.scheme == 'http')
                or not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment):
            raise AnalysisError('Use an HTTPS base URL, or explicitly allow loopback HTTP')
        # Both limits are client decisions, not model properties: a thinking model can take
        # minutes, and an answer with many cited facts needs room. They are parameters with
        # hard caps so a slow or verbose model fails loudly instead of being mislabelled.
        if not model or not 0 < timeout <= 180:
            raise AnalysisError('Model and bounded timeout are required')
        if type(max_tokens) is not int or not 200 <= max_tokens <= 8000:
            raise AnalysisError('max_tokens must be between 200 and 8000')
        self.max_tokens = max_tokens
        self.base = base_url.rstrip('/')
        self.model = model
        self.embedding_model = embedding_model
        self.key = os.environ.get(key_env, '')
        if not self.key and not (local and allow_local_http):
            raise AnalysisError('Model credential environment variable is not set')
        self.timeout = timeout
        self.identity = 'compatible:' + digest([self.base, model])
        self.embedding_identity = 'embedding:' + digest([self.base, embedding_model])
        # The reranker is a second stage over the fused list, so its identity matters for
        # invalidation the same way the embedding model does.
        self.rerank_model = rerank_model
        self.rerank_identity = 'rerank:' + digest([self.base, rerank_model]) if rerank_model else ''
        # Reranking is not part of the OpenAI-compatible surface: some vendors serve /rerank
        # next to the chat endpoint, others (DashScope) serve a native path with a different
        # request and response envelope. The path is configuration; the envelope is chosen
        # from the path rather than hardcoded to one vendor.
        self.rerank_path = rerank_path
        self.origin = parsed.scheme + '://' + parsed.netloc
        self.usage = []

    def post(self, endpoint, payload):
        url = endpoint if endpoint.startswith('http') else self.base + endpoint
        request = Request(url, data=canonical(payload).encode(),
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
        payload = dict(model=self.model, messages=messages, temperature=0,
                       max_tokens=self.max_tokens)
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

    def rerank(self, query, documents):
        """Second-stage scoring: return [(index, score), ...] for every document.

        A reranker that returns a partial ranking, a bad index or a non-numeric score is a
        failure, not a hint: the caller degrades to the fused order and says so.
        """
        if not self.rerank_model:
            raise AnalysisError('A rerank model is required for two-stage retrieval')
        if not documents or len(documents) > 64 or not isinstance(query, str) or len(query) > 4000:
            raise AnalysisError('Invalid rerank request')
        for document in documents:
            if not isinstance(document, str) or len(document) > 8000:
                raise AnalysisError('Rerank documents must be bounded strings')
        native = self.rerank_path.startswith('/api/') or self.rerank_path.startswith('http')
        if native:
            # DashScope-style envelope: input/parameters in, output.results out.
            payload = dict(model=self.rerank_model,
                           input=dict(query=query, documents=documents),
                           parameters=dict(return_documents=False, top_n=len(documents)))
            endpoint = (self.rerank_path if self.rerank_path.startswith('http')
                        else self.origin + self.rerank_path)
        else:
            payload = dict(model=self.rerank_model, query=query, documents=documents,
                           top_n=len(documents))
            endpoint = self.rerank_path
        response = self.post(endpoint, payload)
        results = None
        if isinstance(response, dict):
            results = response.get('results')
            if results is None and isinstance(response.get('output'), dict):
                results = response['output'].get('results')
        if not isinstance(results, list) or len(results) != len(documents):
            raise AnalysisError('Rerank returned an incomplete or malformed ranking')
        ranked = []
        seen = set()
        for entry in results:
            if not isinstance(entry, dict):
                raise AnalysisError('Invalid rerank result')
            index, score = entry.get('index'), entry.get('relevance_score')
            if type(index) is not int or not 0 <= index < len(documents) or index in seen:
                raise AnalysisError('Invalid rerank index')
            if type(score) not in (int, float):
                raise AnalysisError('Invalid rerank score')
            seen.add(index)
            ranked.append((index, float(score)))
        ranked.sort(key=lambda pair: (-pair[1], pair[0]))
        return ranked


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
            facts=[dict(statement='执行报告包含以下记录。',
                        citations=[line_reference(item)])],
            hypotheses=[], missing_info=['需配置真实模型后评估诊断质量。'],
            next_steps=[dict(action='检查报告和日志中的原始证据。', expected_observation='确认执行状态。')])
        return dict(role='assistant', content=canonical(analysis))
