#!/usr/bin/env python3
"""Serve one transient HTTP failure followed by a fixed archive payload."""

from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys
import threading


PAYLOAD = Path(sys.argv[1]).read_bytes()


class RetryHandler(BaseHTTPRequestHandler):
    requests = 0

    def do_GET(self):
        type(self).requests += 1
        request = type(self).requests
        print(f"REQUEST {request}", flush=True)
        if request == 1:
            self.send_response(503)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        self.wfile.write(PAYLOAD)
        threading.Thread(target=self.server.shutdown, daemon=True).start()

    def log_message(self, _format, *_args):
        return


server = HTTPServer(("127.0.0.1", 0), RetryHandler)
print(f"PORT {server.server_port}", flush=True)
server.serve_forever()
server.server_close()
