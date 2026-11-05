"""Scoped SQLite snapshots, conversation records and rebuildable hybrid retrieval."""
from collections import Counter
import json
import math
from pathlib import Path
import re
import sqlite3
import uuid

from .models import AnalysisError, Bundle, Evidence, canonical


def tokens(text):
    words = re.findall(r'[a-z0-9_./:-]+|[\u4e00-\u9fff]', text.lower())
    return words + [a+b for a, b in zip(words, words[1:])
                    if len(a) == len(b) == 1 and '\u4e00' <= a <= '\u9fff' and '\u4e00' <= b <= '\u9fff']


def normalize(vector):
    if not vector or len(vector) > 8192 or any(type(v) not in (int, float) or not math.isfinite(v) for v in vector):
        raise AnalysisError('Invalid embedding vector')
    norm = math.sqrt(sum(float(v)*v for v in vector))
    if not norm or not math.isfinite(norm):
        raise AnalysisError('Invalid embedding norm')
    return [v/norm for v in vector]


class Store:
    def __init__(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.db = sqlite3.connect(path, timeout=10)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS bundles(scope TEXT, run TEXT, body TEXT,
            PRIMARY KEY(scope,run));
          CREATE TABLE IF NOT EXISTS evidence(id TEXT PRIMARY KEY, scope TEXT, run TEXT, body TEXT);
          CREATE INDEX IF NOT EXISTS evidence_scope ON evidence(scope,run);
          CREATE TABLE IF NOT EXISTS vectors(id TEXT, model TEXT, vector TEXT, PRIMARY KEY(id,model));
          CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY, scope TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS turns(id INTEGER PRIMARY KEY AUTOINCREMENT,
            session TEXT, scope TEXT, body TEXT);
          CREATE TABLE IF NOT EXISTS analyses(id TEXT PRIMARY KEY, scope TEXT, cache_key TEXT, body TEXT);
          CREATE TABLE IF NOT EXISTS verifications(scope TEXT, analysis_id TEXT, proposal_id TEXT,
            run_id TEXT, body TEXT, PRIMARY KEY(scope,analysis_id,proposal_id,run_id));
          CREATE INDEX IF NOT EXISTS verifications_scope ON verifications(scope,analysis_id);
        ''')

    def close(self):
        self.db.close()

    def put_bundle(self, bundle):
        with self.db:
            self.db.execute('DELETE FROM vectors WHERE id IN (SELECT id FROM evidence WHERE scope=? AND run=?)',
                            (bundle.scope, bundle.run_id))
            self.db.execute('DELETE FROM evidence WHERE scope=? AND run=?', (bundle.scope, bundle.run_id))
            self.db.execute('INSERT OR REPLACE INTO bundles VALUES(?,?,?)',
                            (bundle.scope, bundle.run_id, canonical(bundle.json())))
            self.db.executemany('INSERT INTO evidence VALUES(?,?,?,?)',
                               [(e.id, e.scope, e.run_id, canonical(e.json())) for e in bundle.evidence])

    def bundle(self, scope, run_id):
        row = self.db.execute('SELECT body FROM bundles WHERE scope=? AND run=?', (scope, run_id)).fetchone()
        if not row:
            raise AnalysisError('Run is not registered in this project scope')
        return Bundle.load(json.loads(row[0]))

    def corpus(self, scope):
        rows = self.db.execute('SELECT body FROM evidence WHERE scope=? ORDER BY id LIMIT 10001', (scope,)).fetchall()
        if len(rows) > 10000:
            raise AnalysisError('Local index exceeds 10000 chunks; split project scope')
        return [Evidence(**json.loads(row[0])) for row in rows]

    def get(self, scope, evidence_id):
        row = self.db.execute('SELECT body FROM evidence WHERE scope=? AND id=?', (scope, evidence_id)).fetchone()
        if not row:
            raise AnalysisError('Evidence is unavailable in this scope')
        return Evidence(**json.loads(row[0]))

    def embed(self, scope, provider):
        items = self.corpus(scope)
        model = provider.embedding_identity
        # Compute before transaction; on error the old index remains intact.
        vectors = [normalize(provider.embed(item.text)) for item in items]
        if len({len(v) for v in vectors}) > 1:
            raise AnalysisError('Embedding dimensions changed during indexing')
        with self.db:
            for item, vector in zip(items, vectors):
                self.db.execute('INSERT OR REPLACE INTO vectors VALUES(?,?,?)', (item.id, model, canonical(vector)))
        return len(items)

    def search(self, scope, query, limit=6, provider=None, exclude_run=None):
        if not 1 <= limit <= 12 or not isinstance(query, str) or len(query) > 4000:
            raise AnalysisError('Invalid search budget')
        items = [e for e in self.corpus(scope) if e.run_id != exclude_run]
        if not items:
            return []
        terms = set(tokens(query))
        counts = [Counter(tokens(e.text)) for e in items]
        frequencies = {term: sum(term in c for c in counts) for term in terms}
        average = sum(sum(c.values()) for c in counts) / len(counts) or 1
        lexical = []
        for i, count in enumerate(counts):
            score = 0.0
            for term in terms:
                tf = count[term]
                if tf:
                    df = frequencies[term]
                    idf = math.log(1 + (len(items)-df+0.5)/(df+0.5))
                    score += idf * tf * 2.2 / (tf+1.2*(0.25+0.75*sum(count.values())/average))
            if score:
                lexical.append((i, score))
        lexical.sort(key=lambda x: (-x[1], items[x[0]].id))
        semantic = []
        if provider:
            query_vec = normalize(provider.embed(query))
            vectors = dict(self.db.execute('SELECT id,vector FROM vectors WHERE model=?',
                                          (provider.embedding_identity,)).fetchall())
            for i, item in enumerate(items):
                if item.id not in vectors:
                    raise AnalysisError('Embedding index incomplete for this model; run --semantic index')
                vector = json.loads(vectors[item.id])
                if len(vector) != len(query_vec):
                    raise AnalysisError('Embedding dimension mismatch; rebuild index')
                score = sum(a*b for a, b in zip(query_vec, vector))
                if score > 0:
                    semantic.append((i, score))
            semantic.sort(key=lambda x: (-x[1], items[x[0]].id))
        fused = Counter()
        for ranking in (lexical, semantic):
            for rank, (i, _) in enumerate(ranking[:60], 1):
                fused[i] += 1/(60+rank)
        return [dict(evidence=items[i].json(), score=score,
                     method='hybrid_rrf' if provider else 'lexical_bm25')
                for i, score in sorted(fused.items(), key=lambda x: (-x[1], items[x[0]].id))[:limit]]

    def session(self, scope, session_id=None):
        if session_id is None:
            session_id = 'chat-' + uuid.uuid4().hex
            with self.db:
                self.db.execute('INSERT INTO sessions VALUES(?,?)', (session_id, scope))
        row = self.db.execute('SELECT scope FROM sessions WHERE id=?', (session_id,)).fetchone()
        if not row or row[0] != scope:
            raise AnalysisError('Conversation is unavailable in this scope')
        return session_id

    def history(self, scope, session_id):
        self.session(scope, session_id)
        rows = self.db.execute('SELECT body FROM turns WHERE session=? AND scope=? ORDER BY id DESC LIMIT 3',
                               (session_id, scope)).fetchall()
        return [json.loads(row[0]) for row in reversed(rows)]

    def remember(self, scope, session_id, question, result):
        self.session(scope, session_id)
        # Old conclusions are conversation context, never direct evidence.
        body = dict(question=question, analysis_id=result['analysis_id'], run_id=result['run_id'],
                    summary=(result.get('analysis') or {}).get('summary', '')[:1500],
                    status=result['status'])
        with self.db:
            self.db.execute('INSERT INTO turns(session,scope,body) VALUES(?,?,?)',
                            (session_id, scope, canonical(body)))

    def cached(self, scope, key):
        row = self.db.execute('SELECT body FROM analyses WHERE scope=? AND cache_key=? ORDER BY rowid DESC LIMIT 1',
                              (scope, key)).fetchone()
        if row:
            value = json.loads(row[0])
            if value['status'] == 'completed':
                return value

    def save_analysis(self, scope, key, result):
        with self.db:
            self.db.execute('INSERT INTO analyses VALUES(?,?,?,?)',
                            (result['analysis_id'], scope, key, canonical(result)))

    def analyses(self, scope):
        rows = self.db.execute('SELECT body FROM analyses WHERE scope=? ORDER BY rowid',
                               (scope,)).fetchall()
        return [json.loads(row[0]) for row in rows]

    # A verification run is what turns a proposal into a fact: it has to be reachable from
    # the conversation that proposed it, otherwise the next analyze can only guess that the
    # new run exists and what it was for.
    def save_verification(self, scope, receipt):
        with self.db:
            self.db.execute('INSERT OR REPLACE INTO verifications VALUES(?,?,?,?,?)',
                            (scope, receipt['analysis_id'], receipt['proposal_id'],
                             receipt['run_id'], canonical(receipt)))

    def verifications(self, scope, analysis_ids, limit=4):
        if not analysis_ids:
            return []
        marks = ','.join('?' * len(analysis_ids))
        rows = self.db.execute(
            'SELECT body FROM verifications WHERE scope=? AND analysis_id IN (' + marks + ') '
            'ORDER BY rowid DESC LIMIT ?',
            (scope, *analysis_ids, limit)).fetchall()
        return [json.loads(row[0]) for row in reversed(rows)]
