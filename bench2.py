import sys, time, tempfile, json, random
sys.path.insert(0, r"C:\Users\lipinzheng\documents\deepseek-harness\default-workspace\sbx")
from pathlib import Path
from sandbox_ai.store import Store, encode_vector
from sandbox_ai.models import evidence, canonical

DIM = 1024
class FakeProvider:
    embedding_identity = "fake:1024"
    def embed(self, text):
        return [random.random() for _ in range(DIM)]

with tempfile.TemporaryDirectory() as temp:
    store = Store(Path(temp)/"b.db")
    n = 1000
    random.seed(7)
    items = [evidence("p", "run-%d" % i, "log", "stdout.bin", "chars 0:200",
                      "error line about file %d\n" % i * 5) for i in range(n)]
    with store.db:
        store.db.executemany("INSERT INTO evidence VALUES(?,?,?,?)",
                             [(e.id, e.scope, e.run_id, canonical(e.json())) for e in items])
        store.db.executemany("INSERT INTO vectors VALUES(?,?,?)",
                             [(e.id, FakeProvider.embedding_identity,
                               encode_vector([random.random() for _ in range(DIM)])) for e in items])
    store._generation += 1
    provider = FakeProvider()
    for label in ("first (decode blobs)", "second (cached matrix)"):
        start = time.perf_counter()
        hits = store.search("p", "why did file 733 fail", limit=8, provider=provider)
        print(label + ":", round((time.perf_counter()-start)*1000, 1), "ms,", len(hits), "hits")
    print("database size:", round((Path(temp)/"b.db").stat().st_size/1024/1024, 2), "MiB for", n, "vectors")
    store.close()
