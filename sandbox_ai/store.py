"""Scoped SQLite snapshots, conversation records and rebuildable hybrid retrieval."""
from collections import Counter
import json
import math
from operator import mul
from pathlib import Path
import re
import sqlite3
import uuid

from .models import AnalysisError, Bundle, Evidence, canonical
try:
    from array import array
except ImportError:  # pragma: no cover
    array = None

try:
    import fcntl
except ImportError:  # Windows: no flock, so no cross-process guard
    fcntl = None


def _lock(path):
    """Exclusive advisory lock next to the database, released on close."""
    if fcntl is None:
        return None
    handle = open(str(path) + '.lock', 'w')
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        raise AnalysisError('This database is already open in another process; one writer is allowed')
    return handle


def encode_vector(vector):
    """float32 blob: ~4x smaller than a JSON array and parsed without a JSON pass."""
    if array is None:                     # pragma: no cover - array ships with CPython
        return canonical([float(v) for v in vector])
    return array('f', [float(v) for v in vector]).tobytes()


def decode_vector(value):
    """Accept both layouts: blobs written now, JSON text from older databases."""
    if isinstance(value, (bytes, bytearray, memoryview)):
        if array is None:                 # pragma: no cover
            raise AnalysisError('Vector binary format needs the array module')
        decoded = array('f')
        decoded.frombytes(bytes(value))
        return decoded
    return array('f', json.loads(value)) if array is not None else json.loads(value)


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


SCHEMA_VERSION = 1


class Store:
    def __init__(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        # One writer at a time. WAL keeps readers going, but two writers on the same file can
        # interleave imports; a lock file makes that a clear error instead of a duplicate row.
        self._lock = _lock(path)
        self.db = sqlite3.connect(path, timeout=10)
        # Reading a scope's corpus means parsing every evidence row; every query and every
        # search_history tool call used to do that. The cache is dropped by a generation
        # counter whenever the corpus changes, so it cannot serve a stale answer.
        self._generation = 0
        self._corpus = {}
        self._lexical = {}
        self._vectors = {}
        self.db.execute('PRAGMA journal_mode=WAL')
        # A database written by a newer version of this code is refused rather than migrated
        # by guesswork: silently writing into a schema we do not understand is how data is lost.
        version = self.db.execute('PRAGMA user_version').fetchone()[0]
        if version > SCHEMA_VERSION:
            self.db.close()
            self._release_lock()
            raise AnalysisError('This database was written by a newer version (schema ' +
                                str(version) + ' > ' + str(SCHEMA_VERSION) +
                                '); upgrade the tool or use another file')
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
          CREATE TABLE IF NOT EXISTS hypothesis_status(scope TEXT, statement TEXT, status TEXT,
            run_id TEXT, analysis_id TEXT, body TEXT);
          CREATE INDEX IF NOT EXISTS hypothesis_scope ON hypothesis_status(scope);
        ''')
        # A database written before this index existed may hold duplicate rows, and creating a
        # UNIQUE index over them raises - which would make the whole library fail to open an
        # older file. Collapse the duplicates first, then add the constraint.
        try:
            self.db.execute('CREATE UNIQUE INDEX IF NOT EXISTS hypothesis_unique '
                            'ON hypothesis_status(scope,statement,status,run_id)')
        except sqlite3.IntegrityError:
            with self.db:
                self.db.execute('DELETE FROM hypothesis_status WHERE rowid NOT IN '
                                '(SELECT MIN(rowid) FROM hypothesis_status '
                                'GROUP BY scope,statement,status,run_id)')
            self.db.execute('CREATE UNIQUE INDEX IF NOT EXISTS hypothesis_unique '
                            'ON hypothesis_status(scope,statement,status,run_id)')
        if version < SCHEMA_VERSION:
            self.db.execute('PRAGMA user_version = ' + str(SCHEMA_VERSION))

    def _release_lock(self):
        if self._lock is not None:
            try:
                fcntl.flock(self._lock.fileno(), fcntl.LOCK_UN)
            except (OSError, AttributeError):
                pass
            self._lock.close()
            self._lock = None

    def close(self):
        self.db.close()
        self._release_lock()

    def put_bundle(self, bundle, force=False):
        """Store a snapshot; skip it when the identical snapshot is already there.

        The skip matters: replacing a bundle deletes that run's vectors, so a command that
        merely *reads* a run (explain, analyze on an imported run) must not silently drop an
        index the operator built. A genuinely changed snapshot still replaces everything,
        and `force` re-imports on request.
        """
        if not force:
            try:
                if self.bundle(bundle.scope, bundle.run_id).fingerprint == bundle.fingerprint:
                    return False
            except AnalysisError:
                pass
        with self.db:
            self.db.execute('DELETE FROM vectors WHERE id IN (SELECT id FROM evidence WHERE scope=? AND run=?)',
                            (bundle.scope, bundle.run_id))
            self.db.execute('DELETE FROM evidence WHERE scope=? AND run=?', (bundle.scope, bundle.run_id))
            self.db.execute('INSERT OR REPLACE INTO bundles VALUES(?,?,?)',
                            (bundle.scope, bundle.run_id, canonical(bundle.json())))
            self.db.executemany('INSERT INTO evidence VALUES(?,?,?,?)',
                               [(e.id, e.scope, e.run_id, canonical(e.json())) for e in bundle.evidence])
        self._generation += 1
        return True

    def corpus_version(self, scope):
        """A cheap corpus identity for cache keys: it changes whenever evidence changes."""
        return self._generation

    def bundle(self, scope, run_id):
        row = self.db.execute('SELECT body FROM bundles WHERE scope=? AND run=?', (scope, run_id)).fetchone()
        if not row:
            raise AnalysisError('Run is not registered in this project scope')
        return Bundle.load(json.loads(row[0]))

    def corpus(self, scope):
        cached = self._corpus.get(scope)
        if cached is not None and cached[0] == self._generation:
            return list(cached[1])
        rows = self.db.execute('SELECT body FROM evidence WHERE scope=? ORDER BY id LIMIT 10001', (scope,)).fetchall()
        if len(rows) > 10000:
            raise AnalysisError('Local index exceeds 10000 chunks; split project scope')
        items = [Evidence(**json.loads(row[0])) for row in rows]
        self._corpus[scope] = (self._generation, items)
        return list(items)

    def lexical(self, scope, exclude_run=None):
        """Token counts, lengths and postings for a generation.

        Every query used to re-tokenize the whole corpus and recompute document
        frequencies; at a few thousand chunks that cost tens of milliseconds per query,
        and an analysis can issue several. The statistics only change when the corpus
        does, so they are cached against the same generation counter as the corpus.
        """
        key = (scope, exclude_run)
        cached = self._lexical.get(key)
        if cached is not None and cached[0] == self._generation:
            return cached[1]
        items = [e for e in self.corpus(scope)
                 if exclude_run is None or e.run_id != exclude_run]
        counts, lengths, postings = [], [], {}
        for index, item in enumerate(items):
            count = Counter(tokens(item.text))
            counts.append(count)
            lengths.append(sum(count.values()))
            for term in count:
                postings.setdefault(term, []).append(index)
        stats = dict(items=items, counts=counts, lengths=lengths, postings=postings,
                     average=(sum(lengths) / len(lengths)) if lengths else 1)
        self._lexical[key] = (self._generation, stats)
        return stats

    def matrix(self, model):
        """Vectors for one model, decoded once per generation instead of once per query."""
        cached = self._vectors.get(model)
        if cached is not None and cached[0] == self._generation:
            return cached[1]
        rows = self.db.execute('SELECT id,vector FROM vectors WHERE model=?', (model,)).fetchall()
        vectors = {row[0]: decode_vector(row[1]) for row in rows}
        self._vectors[model] = (self._generation, vectors)
        return vectors

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
                self.db.execute('INSERT OR REPLACE INTO vectors VALUES(?,?,?)',
                                (item.id, model, encode_vector(vector)))
        return len(items)

    def search(self, scope, query, limit=6, provider=None, exclude_run=None):
        if not 1 <= limit <= 12 or not isinstance(query, str) or len(query) > 4000:
            raise AnalysisError('Invalid search budget')
        stats = self.lexical(scope, exclude_run)
        items = stats['items']
        if not items:
            return []
        terms = set(tokens(query))
        counts, lengths, postings = stats['counts'], stats['lengths'], stats['postings']
        average = stats['average'] or 1
        candidates = set()
        for term in terms:
            candidates.update(postings.get(term, ()))
        total = len(items)
        lexical = []
        for i in candidates:
            count = counts[i]
            score = 0.0
            for term in terms:
                tf = count.get(term)
                if tf:
                    df = len(postings[term])
                    idf = math.log(1 + (total-df+0.5)/(df+0.5))
                    score += idf * tf * 2.2 / (tf+1.2*(0.25+0.75*lengths[i]/average))
            if score:
                lexical.append((i, score))
        lexical.sort(key=lambda x: (-x[1], items[x[0]].id))
        semantic = []
        if provider:
            query_vec = normalize(provider.embed(query))
            vectors = self.matrix(provider.embedding_identity)
            for i, item in enumerate(items):
                vector = vectors.get(item.id)
                if vector is None:
                    raise AnalysisError('Embedding index incomplete for this model; run --semantic index')
                if len(vector) != len(query_vec):
                    raise AnalysisError('Embedding dimension mismatch; rebuild index')
                score = sum(map(mul, query_vec, vector))
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

    # A hypothesis is only settled if the outcome is recorded somewhere other than a
    # conversation: "run X supported hypothesis H" becomes a row, and a later turn can see it.
    def save_hypothesis_updates(self, scope, analysis_id, updates):
        """Record one row per (statement, status, runs) so repeats do not pile up."""
        with self.db:
            for update in updates:
                body = dict(update, analysis_id=analysis_id, scope=scope)
                self.db.execute('INSERT OR REPLACE INTO hypothesis_status VALUES(?,?,?,?,?,?)',
                                (scope, str(update['statement'])[:500], str(update['status']),
                                 ','.join(sorted(set(update.get('run_ids') or [])
                                                | ({update['run_id']} if update.get('run_id') else set()))),
                                 analysis_id, canonical(body)))

    def hypothesis_history(self, scope, run_ids=None, limit=8):
        """Recent settlements, optionally only those about a given set of runs.

        `run_ids` is how a conversation sees its own history instead of the whole project's:
        a scope can hold hypotheses from unrelated work.
        """
        rows = self.db.execute('SELECT body FROM hypothesis_status WHERE scope=? '
                               'ORDER BY rowid DESC LIMIT 200', (scope,)).fetchall()
        items = [json.loads(row[0]) for row in rows]
        if run_ids is not None:
            wanted = set(run_ids)
            items = [item for item in items
                     if wanted & set(filter(None, str(item.get('run_id') or '').split(',')))]
        return list(reversed(items))[:limit]

    # A verification run is what turns a proposal into a fact: it has to be reachable from
    # the conversation that proposed it, otherwise the next analyze can only guess that the
    # new run exists and what it was for.
    def save_verification(self, scope, receipt):
        with self.db:
            self.db.execute('INSERT OR REPLACE INTO verifications VALUES(?,?,?,?,?)',
                            (scope, receipt['analysis_id'], receipt['proposal_id'],
                             receipt['run_id'], canonical(receipt)))

    def verifications_by_session(self, scope, session_id, limit=4):
        """Every verification this conversation ran, however many turns ago.

        `verifications()` is keyed by the last three turns' analyses, so a receipt older than
        that window silently stops feeding the loop: the model just stops seeing that a check
        was executed. This walks the whole session instead.
        """
        rows = self.db.execute('SELECT body FROM turns WHERE session=? AND scope=?',
                               (session_id, scope)).fetchall()
        ids = []
        for row in rows:
            try:
                analysis_id = json.loads(row[0]).get('analysis_id')
            except ValueError:
                continue
            if analysis_id:
                ids.append(analysis_id)
        if not ids:
            return []
        marks = ','.join('?' * len(ids))
        rows = self.db.execute(
            'SELECT body FROM verifications WHERE scope=? AND analysis_id IN (' + marks + ') '
            'ORDER BY rowid DESC LIMIT ?', [scope] + ids + [limit]).fetchall()
        return [json.loads(row[0]) for row in rows]

    def verifications(self, scope, analysis_ids, limit=4):
        if not analysis_ids:
            return []
        marks = ','.join('?' * len(analysis_ids))
        rows = self.db.execute(
            'SELECT body FROM verifications WHERE scope=? AND analysis_id IN (' + marks + ') '
            'ORDER BY rowid DESC LIMIT ?',
            (scope, *analysis_ids, limit)).fetchall()
        return [json.loads(row[0]) for row in reversed(rows)]
