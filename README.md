# LLM Caching Proxy

A drop-in cache for local LLMs, written in C. Point your app at the proxy instead of Ollama, and repeated requests come back from memory in under a millisecond instead of re-running the model.

No code changes: the proxy speaks the same API as your LLM server and returns its responses byte for byte.

```
your app  ──►  proxy :8080  ──►  Ollama :11434
                  │
                  └── same request again? answered from memory
```

## Try it in 2 minutes

You need [Ollama](https://ollama.com) running with any model, and a Mac or Linux terminal.

1. Download and build the proxy:

   ```bash
   git clone https://github.com/noorhusaan302-png/llm-caching-proxy.git
   cd llm-caching-proxy
   gcc -O2 -pthread -o proxy main.c
   ./proxy
   ```

2. In a second terminal, send the same prompt twice (swap in a model you have, e.g. `llama3.2`):

   ```bash
   curl -i http://localhost:8080/api/generate \
     -d '{"model":"llama3.2","prompt":"What is a mutex?","stream":false}'
   ```

3. The first reply says `X-Cache: MISS` and takes as long as the model. The second says `X-Cache: HIT` and comes back instantly.

To use it in your own app, change the port from `11434` to `8080`. Nothing else changes.

Questions or bugs? [Open an issue](https://github.com/noorhusaan302-png/llm-caching-proxy/issues).

## Why

Local models are slow, and apps repeat themselves: the same system prompt, the same test prompts during development, the same question from different users. Each repeat costs seconds of GPU time. This proxy remembers the answer.

## Results

| Metric | Result | Setup |
| :--- | :--- | :--- |
| Cache hit latency | about 0.5 ms (vs 1,000 ms for the model) | Mock server with a 1 s delay |
| Cached throughput | about 5,000 requests/sec, 0 errors | 1,000 requests from 50 concurrent clients |
| 20 concurrent 1-second LLM calls | about 1 s total instead of 20 s | Mock server |

Reproduce with `python3 tests/benchmark.py --mock`.

## Quick start

**1. Run the proxy**

```bash
gcc -O2 -pthread -o proxy main.c
./proxy
```

Or with Docker:

```bash
docker build -t llm-caching-proxy .
docker run -p 8080:8080 --add-host=host.docker.internal:host-gateway llm-caching-proxy
```

**2. Point your app at port 8080 instead of 11434**

```bash
curl http://localhost:8080/api/generate \
  -d '{"model":"llama3.2","prompt":"What is a mutex?","stream":false}'
```

Run it twice. The second reply is instant and carries the header `X-Cache: HIT`.

It works the same way with the OpenAI-style API:

```python
from openai import OpenAI
client = OpenAI(base_url="http://localhost:8080/v1", api_key="ollama")
```

## What gets cached

| Request | Behavior |
| :--- | :--- |
| `POST /api/generate`, `/api/chat` (Ollama) | Cached |
| `POST /v1/chat/completions`, `/v1/completions` (OpenAI-compatible) | Cached |
| Everything else (`/api/tags`, `/api/pull`, ...) | Passed through, never cached |

- **Cache key:** the path plus the exact request body. The model, prompt, messages and options all have to match, so the same prompt sent to two models gets two entries.
- **Streaming works.** A streamed response is forwarded live as it arrives, recorded, and replayed on the next identical request.
- **Only successful responses are cached.** Errors always go back to the model.
- Every response has an `X-Cache` header: `HIT`, `MISS` or `BYPASS`.

## Configuration

Set with environment variables:

| Variable | Default | Meaning |
| :--- | :--- | :--- |
| `PROXY_PORT` | `8080` | Port the proxy listens on |
| `UPSTREAM` | `127.0.0.1:11434` | Your LLM server, as `host:port` |
| `CACHE_CAPACITY` | `100` | Number of responses kept; the least recently used is evicted |

```bash
UPSTREAM=127.0.0.1:1234 CACHE_CAPACITY=1000 ./proxy   # e.g. LM Studio on port 1234
```

## How it works

- **Thread per connection.** Each client gets its own POSIX thread, so one slow model call doesn't block anyone else.
- **LRU cache.** A doubly linked list keeps entries in order of use, and a hash table finds an entry in O(1). One mutex protects both.
- **Raw sockets.** HTTP is parsed by hand on POSIX stream sockets, with no dependencies beyond libc.

## Tests

The tests run against a fake Ollama server (`tests/mock_ollama.py`), so no GPU or model is needed.

```bash
gcc -O2 -pthread -o proxy main.c
python3 tests/test_proxy.py        # 14 integration tests
python3 tests/benchmark.py --mock  # latency and throughput
```

They cover hits and misses, byte-identical replay, streaming, the Ollama and OpenAI endpoints, pass-through, errors, LRU eviction, 20 parallel misses, and 1,000 requests from 50 clients. The suite also passes under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Limitations

- **Exact match only.** "What is a mutex?" and "what is a mutex" are different entries. There is no semantic matching.
- **Cached answers repeat.** With a temperature above 0 you'd normally get a different answer each time; through the cache you get the first one until it's evicted.
- **In memory only.** The cache is empty after a restart, and entries don't expire by time.
- **HTTP only.** The upstream connection is plain HTTP, which fits local servers. It can't call HTTPS APIs directly.
- **Size limits.** Request bodies up to 8 MB; responses up to 32 MB are cached, larger ones just pass through.
- **A client that disconnects mid-stream** gets no cache entry for that request.

## Project structure

```text
llm-caching-proxy/
├── main.c               # Proxy, worker threads, LRU cache, HTTP handling
├── Dockerfile
├── tests/
│   ├── mock_ollama.py   # Fake Ollama server for testing
│   ├── client.py        # Tiny raw-socket HTTP client used by the tests
│   ├── test_proxy.py    # Integration tests
│   └── benchmark.py     # Latency and throughput benchmark
└── README.md
```
