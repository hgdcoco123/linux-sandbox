import sys, time, tempfile, json, os
sys.path.insert(0, r"C:\Users\lipinzheng\documents\deepseek-harness\default-workspace\sbx")
from pathlib import Path
from sandbox_ai.store import Store
from sandbox_ai.models import evidence

with tempfile.TemporaryDirectory() as temp:
    store = Store(Path(temp)/"b.db")
    n = 2000
    items = [evidence("p", "run-%d" % (i // 20), "log", "stdout.bin",
                      "chars 0:500", ("error code %d while opening file %d\n" % (i, i)) * 12)
             for i in range(n)]
    with store.db:
        store.db.executemany("INSERT INTO evidence VALUES(?,?,?,?)",
                             [(e.id, e.scope, e.run_id, __import__("sandbox_ai.models", fromlist=["canonical"]).canonical(e.json())) for e in items])
    store._generation += 1
    for label in ("first", "second"):
        start = time.perf_counter()
        hits = store.search("p", "why did opening file 1733 fail", limit=8)
        print(label, "search over", n, "chunks:", round((time.perf_counter()-start)*1000, 1), "ms,", len(hits), "hits")
    start = time.perf_counter()
    for _ in range(3):
        store.search("p", "another query about exit codes", limit=8)
    print("3 more queries:", round((time.perf_counter()-start)*1000, 1), "ms")
    store.close()
