import json, socket, time

def ask(prompt, port=8080):
    """Send one prompt to the proxy. Returns (milliseconds, status_line, json_body)."""
    body = json.dumps({"model": "llama3", "prompt": prompt, "stream": False})
    request = (f"POST /api/generate HTTP/1.1\r\nHost: localhost\r\n"
               f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}")
    start = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(request.encode())
    data = b""
    while chunk := s.recv(65536):
        data += chunk
    s.close()
    ms = (time.perf_counter() - start) * 1000
    head, _, payload = data.decode().partition("\r\n\r\n")
    return ms, head.split("\r\n")[0], (json.loads(payload) if payload.startswith("{") else payload)
