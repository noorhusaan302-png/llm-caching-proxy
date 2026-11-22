import json, socket, time

def request(path, payload=None, method="POST", port=8080):
    """Send one raw HTTP request to the proxy.
    Returns (milliseconds, status_line, headers_dict, body_text)."""
    body = json.dumps(payload) if payload is not None else ""
    req = (f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n"
           f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}")
    start = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(req.encode())
    data = b""
    while chunk := s.recv(65536):
        data += chunk
    s.close()
    ms = (time.perf_counter() - start) * 1000
    head, _, text = data.decode().partition("\r\n\r\n")
    lines = head.split("\r\n")
    headers = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
    return ms, lines[0], headers, text

def ask(prompt, model="llama3", port=8080):
    """Non-streaming /api/generate call. Returns (ms, status_line, cache_header, json_body)."""
    ms, status, headers, text = request("/api/generate", {"model": model, "prompt": prompt, "stream": False}, port=port)
    return ms, status, headers.get("x-cache"), (json.loads(text) if text.startswith("{") else text)
