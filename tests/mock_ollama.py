"""A fake Ollama server for testing. It answers like Ollama after a delay,
so we can test the proxy without running a real model."""
import http.server, json, threading, time

class MockOllama:
    def __init__(self, delay=0.5, port=11434):
        self.delay = delay
        self.calls = 0
        mock = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def _json(self, obj):
                out = json.dumps(obj).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def do_GET(self):
                if self.path == "/api/tags":
                    self._json({"models": [{"name": "llama3:latest"}]})
                else:
                    self.send_response(404); self.end_headers()

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                mock.calls += 1
                time.sleep(mock.delay)  # pretend the model is thinking
                model = body.get("model", "mock")
                if self.path == "/api/generate":
                    answer = f'[{model}] He said "hi" to ' + body.get("prompt", "")
                    if body.get("stream", True):        # Ollama streams by default (NDJSON)
                        self.send_response(200)
                        self.send_header("Content-Type", "application/x-ndjson")
                        self.end_headers()
                        for word in answer.split(" "):
                            self.wfile.write((json.dumps({"model": model, "response": word + " ", "done": False}) + "\n").encode())
                            self.wfile.flush()
                        self.wfile.write((json.dumps({"model": model, "response": "", "done": True}) + "\n").encode())
                    else:
                        self._json({"model": model, "response": answer, "done": True})
                elif self.path == "/api/chat":
                    last = body.get("messages", [{}])[-1].get("content", "")
                    self._json({"model": model, "message": {"role": "assistant", "content": f"[{model}] reply to {last}"}, "done": True})
                elif self.path == "/v1/chat/completions":
                    last = body.get("messages", [{}])[-1].get("content", "")
                    self._json({"object": "chat.completion", "model": model,
                                "choices": [{"index": 0, "message": {"role": "assistant", "content": f"[{model}] reply to {last}"}}]})
                elif self.path == "/api/fail":
                    self.send_response(500); self.send_header("Content-Length", "0"); self.end_headers()
                else:
                    self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()

            def log_message(self, *args):
                pass

        class Server(http.server.ThreadingHTTPServer):
            request_queue_size = 128   # default of 5 drops connections under load
            allow_reuse_address = True
        self.server = Server(("127.0.0.1", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
