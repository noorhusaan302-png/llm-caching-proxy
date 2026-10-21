"""Tests for the LLM caching proxy.
Run from the repo folder:   gcc -pthread -o proxy main.c && python3 tests/test_proxy.py
"""
import concurrent.futures as cf, os, subprocess, sys, time, unittest
sys.path.insert(0, os.path.dirname(__file__))
from mock_ollama import MockOllama
from client import ask

PROXY = os.environ.get("PROXY_BIN", "./proxy")
DELAY = 0.5

class ProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ollama = MockOllama(delay=DELAY)
        cls.proc = subprocess.Popen([PROXY], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.ollama.stop()

    def test_miss_then_hit(self):
        _, status, first = ask("what is a mutex?")
        _, status2, second = ask("what is a mutex?")
        self.assertIn("200", status)
        self.assertEqual(first["source"], "ollama_upstream")
        self.assertEqual(second["source"], "cache")
        self.assertEqual(first["response"], second["response"])

    def test_cache_hit_is_fast(self):
        ask("speed test")
        ms, _, body = ask("speed test")
        self.assertEqual(body["source"], "cache")
        self.assertLess(ms, 50)  # a hit should never wait for the model

    def test_quotes_in_answer_stay_valid_json(self):
        _, _, body = ask("quote test")          # would fail to parse if quotes weren't escaped
        self.assertIn('"hi"', body["response"])

    def test_missing_prompt_returns_400(self):
        import socket
        s = socket.create_connection(("127.0.0.1", 8080))
        s.sendall(b"POST /api/generate HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}")
        reply = s.recv(4096).decode(); s.close()
        self.assertIn("400", reply)

    def test_concurrent_misses_run_in_parallel(self):
        start = time.perf_counter()
        with cf.ThreadPoolExecutor(20) as pool:
            results = list(pool.map(lambda i: ask(f"parallel {i}"), range(20)))
        elapsed = time.perf_counter() - start
        self.assertTrue(all("200" in r[1] for r in results))
        # 20 one-at-a-time calls would take 20 * DELAY = 10 s; in parallel it's about 1 * DELAY
        self.assertLess(elapsed, DELAY * 4)

    def test_many_clients_no_errors(self):
        ask("load test")
        with cf.ThreadPoolExecutor(50) as pool:
            results = list(pool.map(lambda i: ask("load test"), range(1000)))
        self.assertEqual(sum("200" not in r[1] for r in results), 0)

    def test_lru_evicts_oldest(self):
        self.ollama.delay = 0.0
        ask("evict-me")
        with cf.ThreadPoolExecutor(20) as pool:   # push 100 newer prompts in
            list(pool.map(lambda i: ask(f"filler {i}"), range(100)))
        _, _, body = ask("evict-me")
        self.ollama.delay = DELAY
        self.assertEqual(body["source"], "ollama_upstream")  # it was pushed out of the cache

if __name__ == "__main__":
    unittest.main(verbosity=2)
