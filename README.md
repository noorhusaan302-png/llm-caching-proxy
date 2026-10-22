# LLM Caching Proxy & AI Inference Accelerator

A multithreaded HTTP proxy in C that sits between client applications and a local LLM backend (Ollama). It caches prompt/response pairs so repeated prompts are answered from memory instead of re-running the model.

## Features

- **Inference middleware:** forwards client requests to a local LLM backend (Ollama on port `11434`) and caches the generated reply.
- **Thread-safe LRU cache:** custom LRU eviction (capacity 100 entries) protected by a POSIX mutex, so concurrent client threads can read and write it safely.
- **Redundant compute elimination:** an exact cache hit returns the stored response immediately with `"source": "cache"`, skipping the model entirely. A miss is forwarded upstream and tagged `"source": "ollama_upstream"`.
- **Raw POSIX networking:** built on stream sockets, listening on port `8080`, one thread per client connection, with HTTP/JSON handling.

## Results

| Metric | Result | Setup |
| :--- | :--- | :--- |
| Cache hit latency | < 1 ms | Local |
| 20 concurrent 1-second LLM calls | ~1 s total instead of ~20 s | Mock server |
| 1,000 requests from 50 concurrent clients | 0 errors | Mock server |

## Quick Start

1. Start the LLM backend:
```bash
   ollama serve
```
2. Build and run the proxy:
```bash
   gcc -O2 -pthread -o proxy main.c
   ./proxy
```
3. Send a prompt (`"stream": false` is required):
```bash
   curl -s http://localhost:8080/api/generate \
     -d '{"model":"llama3","prompt":"What is a mutex?","stream":false}'
```
   Send the same command again. The reply returns in under 1 ms and includes `"source": "cache"`.

## Tests and Benchmark

The tests run against a fake Ollama server (`tests/mock_ollama.py`), so no GPU or model is needed.

```bash
# Hits, misses, JSON escaping, 400 errors, parallel misses, 1,000 requests from 50 clients, LRU eviction
python3 tests/test_proxy.py

# Benchmark against a real running Ollama
python3 tests/benchmark.py

# Benchmark against the mock server (isolates proxy overhead)
python3 tests/benchmark.py --mock
```

## Project Structure

```text
llm-caching-proxy/
├── main.c               # Server entry point, worker threads, LRU cache, socket handling
├── tests/
│   ├── mock_ollama.py   # Fake Ollama server for testing
│   ├── test_proxy.py    # Integration tests
│   └── benchmark.py     # Concurrency and throughput benchmark
└── README.md
```

## Design Notes

- **Cache key:** entries are keyed by the exact prompt text.
- **Deterministic replies:** because replies are cached, repeating a prompt returns the same answer until it is evicted, even if the model would normally vary its output.

## Limitations

- The cache key is the prompt only, so the same prompt sent to different models returns the same cached reply.
- Prompts are limited to 1,023 characters; longer ones are truncated, so two long prompts with the same start would share a cache entry.
- Cache lookup walks a doubly linked list (O(n)). That's fine for 100 entries; a hash map index would be needed for more.
- Streaming responses (`"stream": true`) are not supported.
- Each request is read with a single 8 KB `read()`, so very large requests may be cut off.

## Tech Stack

C (POSIX), pthreads and mutexes, POSIX sockets, HTTP/1.1, Python 3 (tests), Ollama API, GCC/Clang.
