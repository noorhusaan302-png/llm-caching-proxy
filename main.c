// LLM Caching Proxy: a drop-in cache in front of Ollama (or any OpenAI-compatible server).
// Point your app at this proxy instead of Ollama. Repeated requests are answered from
// memory; everything else is passed through untouched.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>

#define HEADER_MAX     16384             // max size of request headers
#define BODY_MAX       (8 * 1024 * 1024) // max request body (8 MB)
#define RESPONSE_MAX   (32 * 1024 * 1024)// max upstream response we will cache (32 MB)
#define BUCKETS        4096              // hash table size for O(1) cache lookups

// ---------------- config (set with environment variables) ----------------
static int  proxy_port = 8080;
static char upstream_host[256] = "127.0.0.1";
static char upstream_port[16]  = "11434";
static int  cache_capacity = 100;

// ---------------- LRU cache: doubly linked list + hash index ----------------
// The list keeps recency order (head = newest). The hash table finds a key in O(1).
typedef struct CacheNode {
    char *key;              // "PATH\nBODY": same request = same key
    char *response;         // the full raw HTTP response from upstream
    size_t response_len;
    uint64_t hash;
    struct CacheNode *prev, *next;   // LRU list
    struct CacheNode *bucket_next;   // hash chain
} CacheNode;

typedef struct {
    int capacity, size;
    CacheNode *head, *tail;
    CacheNode *buckets[BUCKETS];
    pthread_mutex_t lock;
    long hits, misses;
} LRUCache;

static LRUCache cache;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void list_unlink(LRUCache *c, CacheNode *n) {
    if (n->prev) n->prev->next = n->next; else c->head = n->next;
    if (n->next) n->next->prev = n->prev; else c->tail = n->prev;
    n->prev = n->next = NULL;
}

static void list_push_front(LRUCache *c, CacheNode *n) {
    n->prev = NULL;
    n->next = c->head;
    if (c->head) c->head->prev = n;
    c->head = n;
    if (!c->tail) c->tail = n;
}

static CacheNode *bucket_find(LRUCache *c, const char *key, uint64_t h) {
    for (CacheNode *n = c->buckets[h % BUCKETS]; n; n = n->bucket_next)
        if (n->hash == h && strcmp(n->key, key) == 0) return n;
    return NULL;
}

static void bucket_remove(LRUCache *c, CacheNode *target) {
    CacheNode **pp = &c->buckets[target->hash % BUCKETS];
    while (*pp && *pp != target) pp = &(*pp)->bucket_next;
    if (*pp) *pp = target->bucket_next;
}

// Returns a malloc'd copy of the cached response (caller frees), or NULL on a miss.
static char *cache_get(LRUCache *c, const char *key, size_t *len_out) {
    uint64_t h = fnv1a(key);
    pthread_mutex_lock(&c->lock);
    CacheNode *n = bucket_find(c, key, h);
    char *copy = NULL;
    if (n) {
        list_unlink(c, n);            // move to front = most recently used
        list_push_front(c, n);
        copy = malloc(n->response_len);
        if (copy) { memcpy(copy, n->response, n->response_len); *len_out = n->response_len; }
        c->hits++;
    } else {
        c->misses++;
    }
    pthread_mutex_unlock(&c->lock);
    return copy;
}

static void cache_put(LRUCache *c, const char *key, const char *resp, size_t len) {
    uint64_t h = fnv1a(key);
    pthread_mutex_lock(&c->lock);
    CacheNode *n = bucket_find(c, key, h);
    if (n) {                          // already cached (two misses raced): just refresh it
        list_unlink(c, n);
        list_push_front(c, n);
        pthread_mutex_unlock(&c->lock);
        return;
    }
    n = calloc(1, sizeof(CacheNode));
    if (!n) { pthread_mutex_unlock(&c->lock); return; }
    n->key = strdup(key);
    n->response = malloc(len);
    if (!n->key || !n->response) { free(n->key); free(n->response); free(n); pthread_mutex_unlock(&c->lock); return; }
    memcpy(n->response, resp, len);
    n->response_len = len;
    n->hash = h;
    n->bucket_next = c->buckets[h % BUCKETS];
    c->buckets[h % BUCKETS] = n;
    list_push_front(c, n);
    c->size++;

    if (c->size > c->capacity) {      // evict the least recently used entry
        CacheNode *old = c->tail;
        list_unlink(c, old);
        bucket_remove(c, old);
        free(old->key); free(old->response); free(old);
        c->size--;
    }
    pthread_mutex_unlock(&c->lock);
}

// ---------------- small helpers ----------------
static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static void send_error(int fd, const char *status, const char *msg) {
    char out[512];
    int body_len = snprintf(NULL, 0, "{\"error\": \"%s\"}", msg);
    snprintf(out, sizeof(out),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n{\"error\": \"%s\"}",
        status, body_len, msg);
    write_all(fd, out, strlen(out));
}

// Case-insensitive header lookup. Copies the value into dest. Returns 1 if found.
static int get_header(const char *headers, const char *name, char *dest, size_t dest_size) {
    size_t name_len = strlen(name);
    const char *line = strstr(headers, "\r\n");
    while (line && line[2] != '\r') {
        line += 2;
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (*v == ' ') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && i < dest_size - 1) { dest[i] = v[i]; i++; }
            dest[i] = '\0';
            return 1;
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

// Only generation requests are worth caching. Everything else (model lists, pulls...) passes through.
static int is_cacheable(const char *method, const char *path) {
    if (strcmp(method, "POST") != 0) return 0;
    return strcmp(path, "/api/generate") == 0 || strcmp(path, "/api/chat") == 0 ||
           strcmp(path, "/v1/chat/completions") == 0 || strcmp(path, "/v1/completions") == 0;
}

// Does the last non-empty NDJSON line say "done": true? (Ollama's end-of-stream marker.)
static int ndjson_done(const char *body, size_t len) {
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r' || body[len - 1] == ' ')) len--;
    if (len == 0) return 0;
    size_t start = len;
    while (start > 0 && body[start - 1] != '\n') start--;
    const char *line = body + start;
    size_t line_len = len - start;
    for (size_t i = 0; i + 6 <= line_len; i++) {
        if (memcmp(line + i, "\"done\"", 6) != 0) continue;
        size_t j = i + 6;
        while (j < line_len && (line[j] == ' ' || line[j] == ':')) j++;
        return j + 4 <= line_len && memcmp(line + j, "true", 4) == 0;
    }
    return 0;
}

static int contains(const char *hay, size_t len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

// Decodes a chunked body into `out` (caller frees). Returns 1 only if the final 0-size chunk was
// reached, i.e. the upstream really finished sending. Returns 0 for a cut-off or malformed body.
static int dechunk(const char *p, size_t len, char **out, size_t *out_len) {
    char *buf = malloc(len + 1);
    if (!buf) return 0;
    size_t pos = 0, w = 0;
    for (;;) {
        size_t line_end = pos;
        while (line_end + 1 < len && !(p[line_end] == '\r' && p[line_end + 1] == '\n')) line_end++;
        if (line_end + 1 >= len) break;                       // size line never finished
        char *endp;
        unsigned long size = strtoul(p + pos, &endp, 16);
        if (endp == p + pos) break;                           // not a hex size
        pos = line_end + 2;
        if (size == 0) { *out = buf; *out_len = w; return 1; } // final chunk: body is complete
        if (pos + size + 2 > len) break;                      // chunk data was cut off
        memcpy(buf + w, p + pos, size);
        w += size;
        pos += size + 2;                                      // skip data and its CRLF
    }
    free(buf);
    return 0;
}

// Only a response we can prove is finished may be cached. A 200 status line alone is not enough:
// the upstream can send headers and then drop the connection halfway through the body.
static int response_complete(const char *resp, size_t len) {
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 4 <= len; i++)
        if (memcmp(resp + i, "\r\n\r\n", 4) == 0) { hdr_end = resp + i; break; }
    if (!hdr_end) return 0;
    size_t head_len = (size_t)(hdr_end - resp) + 4;
    char head[HEADER_MAX];
    if (head_len >= sizeof(head)) return 0;
    memcpy(head, resp, head_len);
    head[head_len] = '\0';
    const char *body = resp + head_len;
    size_t body_len = len - head_len;

    char ctype[128] = "", te[64] = "", cl[32] = "";
    get_header(head, "Content-Type", ctype, sizeof(ctype));
    int is_ndjson = strstr(ctype, "ndjson") != NULL;
    int is_sse = strstr(ctype, "event-stream") != NULL;

    char *decoded = NULL;
    size_t decoded_len = 0;
    int ok;
    if (get_header(head, "Transfer-Encoding", te, sizeof(te)) && strstr(te, "chunked")) {
        if (!dechunk(body, body_len, &decoded, &decoded_len)) return 0;  // final chunk missing
        body = decoded; body_len = decoded_len;
        ok = 1;
    } else if (get_header(head, "Content-Length", cl, sizeof(cl))) {
        char *endp;
        unsigned long want = strtoul(cl, &endp, 10);
        ok = endp != cl && body_len == want;                  // every promised byte arrived
    } else {
        // No framing, the body just ends when the connection closes. A cut-off stream is caught by
        // the marker checks below; for plain JSON, at least require the document to be closed.
        ok = body_len > 0;
        if (ok && !is_ndjson && !is_sse && strstr(ctype, "json")) {
            size_t e = body_len;
            while (e > 0 && (body[e - 1] == '\n' || body[e - 1] == '\r' || body[e - 1] == ' ')) e--;
            ok = e > 0 && (body[e - 1] == '}' || body[e - 1] == ']');
        }
    }
    // A stream also has to reach its own end marker, not just a closed socket.
    if (ok && is_ndjson) ok = ndjson_done(body, body_len);
    if (ok && is_sse) ok = contains(body, body_len, "data: [DONE]");
    free(decoded);
    return ok;
}

static int connect_upstream(void) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(upstream_host, upstream_port, &hints, &res) != 0) return -1;
    int sock = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock); sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

// ---------------- request handling ----------------
static void handle_client(int client) {
    // 1. Read until the end of the headers.
    char head[HEADER_MAX + 1];
    size_t got = 0;
    char *end_of_headers = NULL;
    while (got < HEADER_MAX) {
        ssize_t n = read(client, head + got, HEADER_MAX - got);
        if (n <= 0) break;
        got += (size_t)n;
        head[got] = '\0';
        if ((end_of_headers = strstr(head, "\r\n\r\n"))) break;
    }
    if (!end_of_headers) { send_error(client, "400 Bad Request", "Malformed request"); return; }
    size_t header_len = (size_t)(end_of_headers - head) + 4;

    char method[16] = {0}, path[2048] = {0};
    if (sscanf(head, "%15s %2047s", method, path) != 2) {
        send_error(client, "400 Bad Request", "Malformed request line"); return;
    }

    // 2. Read the whole body using Content-Length (the old version did a single 8 KB read).
    char cl[32] = "0";
    get_header(head, "Content-Length", cl, sizeof(cl));
    long body_len = strtol(cl, NULL, 10);
    if (body_len < 0 || body_len > BODY_MAX) { send_error(client, "413 Payload Too Large", "Request body too large"); return; }

    char *body = malloc((size_t)body_len + 1);
    if (!body) { send_error(client, "500 Internal Server Error", "Out of memory"); return; }
    size_t have = got - header_len;
    if (have > (size_t)body_len) have = (size_t)body_len;
    memcpy(body, head + header_len, have);
    while (have < (size_t)body_len) {
        ssize_t n = read(client, body + have, (size_t)body_len - have);
        if (n <= 0) break;
        have += (size_t)n;
    }
    body[have] = '\0';
    if (have < (size_t)body_len) { free(body); send_error(client, "400 Bad Request", "Incomplete body"); return; }

    // 3. Cache lookup. The key is the path plus the exact body, so the model, prompt,
    //    messages and options all have to match. Different model = different entry.
    int cacheable = is_cacheable(method, path);
    char *key = NULL;
    if (cacheable) {
        key = malloc(strlen(path) + 1 + (size_t)body_len + 1);
        if (key) sprintf(key, "%s\n%s", path, body);
        else cacheable = 0;
    }

    if (cacheable) {
        size_t cached_len = 0;
        char *cached = cache_get(&cache, key, &cached_len);
        if (cached) {
            // Replay the exact upstream response, adding an X-Cache header after the status line.
            char *eol = memchr(cached, '\n', cached_len);
            size_t first_line = eol ? (size_t)(eol - cached) + 1 : 0;
            write_all(client, cached, first_line);
            write_all(client, "X-Cache: HIT\r\n", 14);
            write_all(client, cached + first_line, cached_len - first_line);
            printf("[HIT ] %s %s\n", method, path);
            free(cached); free(key); free(body);
            return;
        }
    }

    // 4. Miss (or not cacheable): forward to upstream. We rebuild the headers so the
    //    upstream closes the connection when done, which tells us the response is complete.
    int up = connect_upstream();
    if (up < 0) {
        send_error(client, "502 Bad Gateway", "Failed to reach the LLM server. Is it running?");
        free(key); free(body); return;
    }
    char ctype[128] = "application/json", auth[1024] = "";
    get_header(head, "Content-Type", ctype, sizeof(ctype));
    int has_auth = get_header(head, "Authorization", auth, sizeof(auth));
    char req_head[4096];
    int rh = snprintf(req_head, sizeof(req_head),
        "%s %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%s%s%sConnection: close\r\n\r\n",
        method, path, upstream_host, upstream_port, ctype, body_len,
        has_auth ? "Authorization: " : "", has_auth ? auth : "", has_auth ? "\r\n" : "");
    if (write_all(up, req_head, (size_t)rh) < 0 || write_all(up, body, (size_t)body_len) < 0) {
        close(up); send_error(client, "502 Bad Gateway", "Upstream write failed");
        free(key); free(body); return;
    }

    // 5. Stream the response to the client as it arrives (so streaming still feels live)
    //    while also recording it, so it can be cached once it's complete.
    char *record = NULL;
    size_t rec_len = 0, rec_cap = 0;
    int recording = cacheable, header_injected = 0, client_gone = 0;
    char chunk[16384];
    ssize_t n;
    while ((n = read(up, chunk, sizeof(chunk))) > 0) {
        if (recording) {
            if (rec_len + (size_t)n > RESPONSE_MAX) { recording = 0; free(record); record = NULL; }
            else {
                if (rec_len + (size_t)n > rec_cap) {
                    rec_cap = (rec_len + (size_t)n) * 2;
                    char *grown = realloc(record, rec_cap);
                    if (!grown) { recording = 0; free(record); record = NULL; }
                    else record = grown;
                }
                if (recording) { memcpy(record + rec_len, chunk, (size_t)n); rec_len += (size_t)n; }
            }
        }
        // Add "X-Cache: MISS" right after the status line on the first chunk.
        if (!header_injected) {
            char *eol = memchr(chunk, '\n', (size_t)n);
            if (eol) {
                size_t first_line = (size_t)(eol - chunk) + 1;
                write_all(client, chunk, first_line);
                write_all(client, cacheable ? "X-Cache: MISS\r\n" : "X-Cache: BYPASS\r\n", cacheable ? 15 : 17);
                if (write_all(client, chunk + first_line, (size_t)n - first_line) < 0) { client_gone = 1; break; }
                header_injected = 1;
                continue;
            }
        }
        if (write_all(client, chunk, (size_t)n) < 0) { client_gone = 1; break; }   // client went away
    }
    int upstream_failed = (n < 0);    // read error from upstream: the response is not trustworthy
    close(up);
    if (!header_injected) {   // upstream accepted the connection but sent nothing back
        send_error(client, "502 Bad Gateway", "Empty response from the LLM server");
        free(record); free(key); free(body); return;
    }

    // 6. Only cache complete, successful responses. Checking the 200 status is not enough: the
    //    upstream may send headers and then disconnect mid-body, so we also check the body really
    //    finished (Content-Length, the final chunk, or the stream's own end marker).
    if (!client_gone && !upstream_failed && recording && record && rec_len > 12 &&
        strncmp(record + 9, "200", 3) == 0 && response_complete(record, rec_len)) {
        cache_put(&cache, key, record, rec_len);
        printf("[MISS] %s %s (cached %zu bytes)\n", method, path, rec_len);
    } else {
        printf("[PASS] %s %s\n", method, path);
    }
    free(record); free(key); free(body);
}

static void *client_thread(void *arg) {
    int client = *(int *)arg;
    free(arg);
    handle_client(client);
    close(client);
    return NULL;
}

static void load_config(void) {
    const char *v;
    if ((v = getenv("PROXY_PORT"))) proxy_port = atoi(v);
    if ((v = getenv("CACHE_CAPACITY")) && atoi(v) > 0) cache_capacity = atoi(v);
    // UPSTREAM can be "host:port", e.g. "127.0.0.1:11434" or "host.docker.internal:11434"
    if ((v = getenv("UPSTREAM"))) {
        const char *colon = strrchr(v, ':');
        if (colon) {
            size_t hl = (size_t)(colon - v);
            if (hl >= sizeof(upstream_host)) hl = sizeof(upstream_host) - 1;
            memcpy(upstream_host, v, hl); upstream_host[hl] = '\0';
            snprintf(upstream_port, sizeof(upstream_port), "%s", colon + 1);
        } else {
            snprintf(upstream_host, sizeof(upstream_host), "%s", v);
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);           // don't crash if a client disconnects early
    setvbuf(stdout, NULL, _IOLBF, 0);   // log lines show up immediately (useful in Docker)
    load_config();
    memset(&cache, 0, sizeof(cache));
    cache.capacity = cache_capacity;
    pthread_mutex_init(&cache.lock, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)proxy_port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 128) < 0) { perror("listen"); return 1; }

    printf("LLM caching proxy on :%d -> upstream %s:%s (cache capacity %d)\n",
           proxy_port, upstream_host, upstream_port, cache_capacity);

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        int *arg = malloc(sizeof(int));
        if (!arg) { close(client); continue; }
        *arg = client;
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) { close(client); free(arg); continue; }
        pthread_detach(tid);   // thread cleans itself up when done
    }
}
