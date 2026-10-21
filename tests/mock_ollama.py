"""A fake Ollama server for testing. It answers /api/generate after a delay,
so we can test the proxy without running a real model."""
import http.server, json, threading, time

class MockOllama:
    def __init__(self, delay=0.5, port=11434):
        self.delay = delay
        self.calls = 0
        mock = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                mock.calls += 1
                time.sleep(mock.delay)  # pretend the model is thinking
                answer = 'He said "hi" to ' + body.get("prompt", "")
                out = json.dumps({"model": "mock", "response": answer, "done": True}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
