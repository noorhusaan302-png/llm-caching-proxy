"""Measures cache-miss vs cache-hit latency and throughput.
Usage: start ./proxy and real Ollama (ollama serve), then:  python3 tests/benchmark.py
Add --mock to use the fake Ollama instead of a real model."""
import concurrent.futures as cf, os, statistics, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from client import ask

if "--mock" in sys.argv:
    from mock_ollama import MockOllama
    MockOllama(delay=1.0)
    time.sleep(0.3)

misses, hits = [], []
for i in range(5):
    p = f"benchmark prompt {i} {time.time()}"
    misses.append(ask(p)[0])
    hits.append(ask(p)[0])
print(f"cache miss (model runs): median {statistics.median(misses):.0f} ms")
print(f"cache hit  (from memory): median {statistics.median(hits):.2f} ms")
print(f"speedup: {statistics.median(misses) / statistics.median(hits):.0f}x")

p = "throughput prompt"; ask(p)
start = time.perf_counter()
with cf.ThreadPoolExecutor(50) as pool:
    results = list(pool.map(lambda i: ask(p), range(1000)))
elapsed = time.perf_counter() - start
errors = sum("200" not in r[1] for r in results)
print(f"1000 cached requests from 50 clients: {1000/elapsed:.0f} req/s, {errors} errors")
