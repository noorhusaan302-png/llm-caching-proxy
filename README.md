# LLM Caching Proxy & AI Inference Accelerator in C

A multi-threaded C HTTP proxy designed to optimize **local Large Language Model (LLM) inference** by intercepting requests, caching prompt-response pairs, and eliminating redundant AI compute.

## 🚀 AI & Systems Architecture

* **AI Inference Middleware:** Acts as an intelligent proxy layer sitting between client applications and a local LLM backend (such as Ollama on port `11434`), cutting down latency and time-to-first-token for repeated prompts.
* **Thread-Safe LRU Cache:** Implements a custom Least Recently Used (LRU) eviction policy protected by POSIX mutexes (`pthread_mutex_t`) to support concurrent multi-client memory lookups without race conditions.
* **Redundant Prompt Elimination:** Automatically serves cached generations on a **Cache Hit**, bypassing expensive transformer prefill computations. On a **Cache Miss**, it forwards the payload upstream and caches the resulting AI output.
* **Raw TCP Networking:** Built entirely from scratch using POSIX sockets (`socket`, `bind`, `listen`, `accept`) on port `8080`, with one POSIX thread per client connection, and HTTP/JSON payloads.

## 🛠️ Tech Stack

* **Systems Language:** C
* **Concurrency:** POSIX Threads (`pthreads`), Mutexes (`pthread_mutex_t`)
* **Networking:** POSIX Sockets, TCP/IP, HTTP & JSON Parsing
* **AI Ecosystem:** Ollama Local Inference API

## ⚙️ Quick Start

1. **Start your local AI backend (Ollama):**
   ```bash
   ollama serve
   ```
2. **Build and run the proxy:**
   ```bash
   gcc -O2 -pthread -o proxy main.c
   ./proxy
   ```
3. **Send a prompt** (the proxy expects `"stream": false`):
   ```bash
   curl -s localhost:8080/api/generate -d '{"model":"llama3","prompt":"What is a mutex?","stream":false}'
   ```
   Send the same prompt again and the reply says `"source": "cache"`.

## 🧪 Tests & Benchmark

The tests use a fake Ollama server (`tests/mock_ollama.py`), so no model is needed.

```bash
gcc -O2 -pthread -o proxy main.c
python3 tests/test_proxy.py          # 7 tests: hit/miss, JSON escaping, 400 errors,
                                     # parallel misses, 1,000 requests from 50 clients, LRU eviction
./proxy &  python3 tests/benchmark.py        # with real Ollama running
./proxy &  python3 tests/benchmark.py --mock # with the fake model
```

## ⚠️ Limitations

* Cache lookup walks a linked list (O(n)); fine for 100 entries, a hash map would be needed for more.
* Streaming responses (`"stream": true`) are not supported.
* Each request is read with a single `read()` call, so very large requests may be cut off.
