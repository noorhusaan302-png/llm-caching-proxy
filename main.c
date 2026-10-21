#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

#define PROXY_PORT 8080
#define OLLAMA_PORT 11434
#define BUFFER_SIZE 8192
#define CACHE_CAPACITY 100   // max prompt/response pairs kept in memory

// --- LRU CACHE STRUCTURES ---
typedef struct CacheNode {
    char *prompt;
    char *response;
    struct CacheNode *prev;
    struct CacheNode *next;
} CacheNode;

typedef struct {
    int capacity;
    int size;
    CacheNode *head;
    CacheNode *tail;
    pthread_mutex_t lock;
} LRUCache;

LRUCache lru_cache;

void init_cache(LRUCache *cache, int capacity) {
    cache->capacity = capacity;
    cache->size = 0;
    cache->head = NULL;
    cache->tail = NULL;
    pthread_mutex_init(&cache->lock, NULL);
}

char* cache_get(LRUCache *cache, const char *prompt) {
    pthread_mutex_lock(&cache->lock);
    CacheNode *curr = cache->head;
    while (curr != NULL) {
        if (strcmp(curr->prompt, prompt) == 0) {
            if (curr != cache->head) {
                if (curr->prev) curr->prev->next = curr->next;
                if (curr->next) curr->next->prev = curr->prev;
                if (curr == cache->tail) cache->tail = curr->prev;

                curr->next = cache->head;
                curr->prev = NULL;
                cache->head->prev = curr;
                cache->head = curr;
            }
            char *resp_copy = strdup(curr->response);
            pthread_mutex_unlock(&cache->lock);
            return resp_copy;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return NULL;
}

void cache_put(LRUCache *cache, const char *prompt, const char *response) {
    pthread_mutex_lock(&cache->lock);
    CacheNode *curr = cache->head;
    while (curr != NULL) {
        if (strcmp(curr->prompt, prompt) == 0) {
            free(curr->response);
            curr->response = strdup(response);
            pthread_mutex_unlock(&cache->lock);
            return;
        }
        curr = curr->next;
    }

    CacheNode *new_node = (CacheNode *)malloc(sizeof(CacheNode));
    new_node->prompt = strdup(prompt);
    new_node->response = strdup(response);
    new_node->prev = NULL;
    new_node->next = NULL;

    if (cache->size == 0) {
        cache->head = new_node;
        cache->tail = new_node;
        cache->size++;
    } else {
        new_node->next = cache->head;
        cache->head->prev = new_node;
        cache->head = new_node;
        cache->size++;

        if (cache->size > cache->capacity) {
            CacheNode *old_tail = cache->tail;
            cache->tail = cache->tail->prev;
            if (cache->tail) cache->tail->next = NULL;
            free(old_tail->prompt);
            free(old_tail->response);
            free(old_tail);
            cache->size--;
        }
    }
    pthread_mutex_unlock(&cache->lock);
}

// --- JSON HELPERS ---
void extract_prompt(const char *json_body, char *dest, size_t dest_size) {
    char *p = strstr(json_body, "\"prompt\"");
    if (!p) { strcpy(dest, ""); return; }
    p = strchr(p, ':'); if (!p) { strcpy(dest, ""); return; }
    p = strchr(p, '"'); if (!p) { strcpy(dest, ""); return; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < dest_size - 1) {
        dest[i++] = *p++;
    }
    dest[i] = '\0';
}

// Extract the "response" text from Ollama's JSON output
void extract_ollama_response(const char *json_body, char *dest, size_t dest_size) {
    char *p = strstr(json_body, "\"response\"");
    if (!p) { 
        strncpy(dest, json_body, dest_size); 
        return; 
    }
    p = strchr(p, ':'); if (!p) { strcpy(dest, ""); return; }
    p = strchr(p, '"'); if (!p) { strcpy(dest, ""); return; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < dest_size - 1) {
        // (an escaped quote \" is handled below, so it doesn't end the string)
        // Handle JSON escapes: \n becomes a space, \" and \\ become the plain character
        if (*p == '\\' && *(p+1) == 'n') {
            dest[i++] = ' ';
            p += 2;
        } else if (*p == '\\' && *(p+1) != '\0') {
            dest[i++] = *(p+1);
            p += 2;
        } else {
            dest[i++] = *p++;
        }
    }
    dest[i] = '\0';
}

// Forward request to local Ollama instance and get response
int forward_to_ollama(const char *raw_request, char *response_dest, size_t dest_size) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(OLLAMA_PORT);
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return -1;
    }

    write(sock, raw_request, strlen(raw_request));

    char ollama_buffer[BUFFER_SIZE * 4];
    memset(ollama_buffer, 0, sizeof(ollama_buffer));
    // Keep reading until Ollama closes the connection (a big answer can arrive in pieces)
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(ollama_buffer) - 1 &&
           (n = read(sock, ollama_buffer + total, sizeof(ollama_buffer) - 1 - total)) > 0) {
        total += n;
    }
    close(sock);

    if (total == 0) return -1;

    // Separate HTTP body from Ollama's response headers
    char *body = strstr(ollama_buffer, "\r\n\r\n");
    body = body ? body + 4 : ollama_buffer;

    extract_ollama_response(body, response_dest, dest_size);
    return 0;
}

// Escape quotes and backslashes so the text is safe to put inside a JSON string
void json_escape(const char *src, char *dest, size_t dest_size) {
    size_t i = 0;
    for (; *src && i + 2 < dest_size; src++) {
        if (*src == '"' || *src == '\\') dest[i++] = '\\';
        dest[i++] = *src;
    }
    dest[i] = '\0';
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);

    ssize_t bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        close(client_socket);
        return;
    }

    char *body = strstr(buffer, "\r\n\r\n");
    body = body ? body + 4 : "";

    char prompt_buffer[1024];
    extract_prompt(body, prompt_buffer, sizeof(prompt_buffer));

    if (strlen(prompt_buffer) == 0) {
        const char *err_resp = "HTTP/1.1 400 Bad Request\r\n\r\n{\"error\": \"No prompt found\"}";
        write(client_socket, err_resp, strlen(err_resp));
        close(client_socket);
        return;
    }

    printf("\n[Request] Prompt: \"%s\"\n", prompt_buffer);

    char *cached_response = cache_get(&lru_cache, prompt_buffer);
    char final_response_buffer[BUFFER_SIZE * 2];

    if (cached_response != NULL) {
        printf(" -> CACHE HIT! Serving instant response from memory.\n");
        char escaped[BUFFER_SIZE];
        json_escape(cached_response, escaped, sizeof(escaped));
        snprintf(final_response_buffer, sizeof(final_response_buffer),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"source\": \"cache\", \"response\": \"%s\"}",
            escaped);
        free(cached_response);
    } else {
        printf(" -> CACHE MISS. Forwarding upstream to Ollama...\n");
        
        char ollama_answer[2048];
        if (forward_to_ollama(buffer, ollama_answer, sizeof(ollama_answer)) != 0) {
            const char *err_resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n{\"error\": \"Failed to reach Ollama. Is 'ollama serve' running?\"}";
            write(client_socket, err_resp, strlen(err_resp));
            close(client_socket);
            return;
        }

        // Save real AI response into cache
        cache_put(&lru_cache, prompt_buffer, ollama_answer);

        char escaped[BUFFER_SIZE];
        json_escape(ollama_answer, escaped, sizeof(escaped));
        snprintf(final_response_buffer, sizeof(final_response_buffer),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"source\": \"ollama_upstream\", \"response\": \"%s\"}",
            escaped);
    }

    write(client_socket, final_response_buffer, strlen(final_response_buffer));
    close(client_socket);
}

// Each client gets its own thread, so a slow LLM call doesn't block other users
void *client_thread(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);
    handle_client(client_socket);
    return NULL;
}

int main() {
    signal(SIGPIPE, SIG_IGN);   // don't crash if a client disconnects early
    init_cache(&lru_cache, CACHE_CAPACITY);

    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PROXY_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("LLM Token-Caching Proxy running on port %d, linked to Ollama on port %d...\n", PROXY_PORT, OLLAMA_PORT);

    while (1) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            continue;
        }
        int *sock_ptr = malloc(sizeof(int));
        if (!sock_ptr) { close(client_socket); continue; }
        *sock_ptr = client_socket;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, sock_ptr) != 0) {
            close(client_socket);
            free(sock_ptr);
            continue;
        }
        pthread_detach(tid);   // thread cleans itself up when done
    }

    return 0;
}
