"""Tests for the LLM caching proxy.
Run from the repo folder:   gcc -O2 -pthread -o proxy main.c && python3 tests/test_proxy.py
"""
import concurrent.futures as cf, json, os, subprocess, sys, time, unittest
sys.path.insert(0, os.path.dirname(__file__))
from mock_ollama import MockOllama
from client import ask, request

PROXY = os.environ.get("PROXY_BIN", "./proxy")
DELAY = 0.5

class ProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ollama = MockOllama(delay=DELAY)
        cls.proc = subprocess.Popen([PROXY], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    env={**os.environ, "CACHE_CAPACITY": "100"})
        time.sleep(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.ollama.stop()

    # --- drop-in behavior: responses look exactly like Ollama's ---
    def test_miss_then_hit_returns_identical_ollama_json(self):
        _, status, c1, first = ask("what is a mutex?")
        _, status2, c2, second = ask("what is a mutex?")
        self.assertIn("200", status)
        self.assertEqual((c1, c2), ("MISS", "HIT"))
        self.assertEqual(first, second)
        self.assertIn("response", first)      # Ollama's own format, no custom fields
        self.assertNotIn("source", first)

    def test_cache_hit_is_fast(self):
        ask("speed test")
        ms, _, cache, _ = ask("speed test")
        self.assertEqual(cache, "HIT")
        self.assertLess(ms, 50)  # a hit should never wait for the model

    def test_quotes_in_answer_stay_valid_json(self):
        _, _, _, body = ask("quote test")       # json.loads would fail if quotes were broken
        self.assertIn('"hi"', body["response"])

    def test_different_model_is_a_different_entry(self):
        _, _, c1, a = ask("same prompt", model="llama3")
        _, _, c2, b = ask("same prompt", model="mistral")
        self.assertEqual((c1, c2), ("MISS", "MISS"))
        self.assertNotEqual(a["response"], b["response"])

    def test_long_prompt_not_truncated(self):
        long_a = "x" * 5000 + "A"
        long_b = "x" * 5000 + "B"
        ask(long_a)
        _, _, cache, body = ask(long_b)
        self.assertEqual(cache, "MISS")       # old version cut prompts at 1,023 chars
        self.assertTrue(body["response"].endswith("B"))

    def test_streaming_is_cached_and_replayed(self):
        payload = {"model": "llama3", "prompt": "stream me"}   # Ollama streams by default
        _, _, h1, t1 = request("/api/generate", payload)
        _, _, h2, t2 = request("/api/generate", payload)
        self.assertEqual((h1["x-cache"], h2["x-cache"]), ("MISS", "HIT"))
        self.assertEqual(t1, t2)
        self.assertIn('"done": true', t2)

    def test_chat_endpoint_cached(self):
        payload = {"model": "llama3", "messages": [{"role": "user", "content": "hello"}], "stream": False}
        _, _, h1, _ = request("/api/chat", payload)
        _, _, h2, t2 = request("/api/chat", payload)
        self.assertEqual((h1["x-cache"], h2["x-cache"]), ("MISS", "HIT"))
        self.assertEqual(json.loads(t2)["message"]["role"], "assistant")

    def test_openai_endpoint_cached(self):
        payload = {"model": "llama3", "messages": [{"role": "user", "content": "hi openai"}]}
        _, _, h1, _ = request("/v1/chat/completions", payload)
        _, _, h2, t2 = request("/v1/chat/completions", payload)
        self.assertEqual((h1["x-cache"], h2["x-cache"]), ("MISS", "HIT"))
        self.assertEqual(json.loads(t2)["object"], "chat.completion")

    def test_other_requests_pass_through(self):
        _, status, h, text = request("/api/tags", method="GET")
        self.assertIn("200", status)
        self.assertEqual(h["x-cache"], "BYPASS")
        self.assertIn("llama3", text)

    def test_errors_are_not_cached(self):
        before = self.ollama.calls
        request("/api/fail", {"x": 1})
        request("/api/fail", {"x": 1})
        self.assertEqual(self.ollama.calls - before, 2)

    # --- concurrency and the cache itself ---
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
        _, _, cache, _ = ask("evict-me")
        self.ollama.delay = DELAY
        self.assertEqual(cache, "MISS")          # it was pushed out of the cache

    def test_upstream_down_returns_502(self):
        self.ollama.stop()
        try:
            _, status, _, _ = ask("nobody home")
            self.assertIn("502", status)
        finally:
            type(self).ollama = MockOllama(delay=DELAY)

if __name__ == "__main__":
    unittest.main(verbosity=2)
