#!/usr/bin/env python3
"""Chunked test origin; a file barrier makes flush checks deterministic."""
import http.server
import pathlib
import re
import sys
import time

root = pathlib.Path(sys.argv[2])

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        match = re.search(r"id=([a-zA-Z0-9_]+)", self.path)
        key = match.group(1) if match else "none"
        body = (root / "stream-body").read_bytes()
        first = body[:4096]
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self.wfile.write(("%x\r\n" % len(first)).encode() + first + b"\r\n")
            self.wfile.flush()
            deadline = time.monotonic() + 8
            while not (root / ("release-" + key)).exists():
                if time.monotonic() > deadline:
                    return
                time.sleep(0.01)
            for offset in range(len(first), len(body), 3073):
                part = body[offset:offset + 3073]
                self.wfile.write(("%x\r\n" % len(part)).encode() + part + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
server.daemon_threads = True
server.serve_forever()
