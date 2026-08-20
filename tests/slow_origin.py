#!/usr/bin/env python3
"""A deliberately slow origin server, for exercising the partial-I/O paths.

Two behaviours the normal `http.server` cannot produce:

  /slow?chunks=N&delay=S   sends the body in N pieces with S seconds between them, so the proxy's
                           read-from-upstream path sees many short reads instead of one big one.
  /trickle?bytes=N         sends N bytes a handful at a time with no Content-Length, ending by
                           closing the connection.

Both exist because a truncated response looks completely fine at small sizes: the bug only appears
when a `send` or `recv` returns less than asked for, which does not happen on a fast loopback with
small payloads.
"""

import argparse
import socketserver
import sys
import time
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse


class SlowHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep test output readable
        pass

    def _query(self):
        return parse_qs(urlparse(self.path).query)

    def do_GET(self):
        path = urlparse(self.path).path
        q = self._query()

        if path == "/slow":
            chunks = int(q.get("chunks", ["10"])[0])
            delay = float(q.get("delay", ["0.05"])[0])
            piece = b"x" * 1024
            body_len = chunks * len(piece)

            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(body_len))
            self.end_headers()
            for _ in range(chunks):
                self.wfile.write(piece)
                self.wfile.flush()
                time.sleep(delay)
            return

        if path == "/trickle":
            total = int(q.get("bytes", ["65536"])[0])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            sent = 0
            while sent < total:
                n = min(512, total - sent)
                self.wfile.write(b"y" * n)
                self.wfile.flush()
                sent += n
                time.sleep(0.002)
            self.close_connection = True
            return

        if path == "/echo_len":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            body = b"0\n"
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_error(404, "not found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        body = f"{len(data)}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18081)
    args = ap.parse_args()

    with ThreadedServer(("127.0.0.1", args.port), SlowHandler) as srv:
        print(f"slow origin on 127.0.0.1:{args.port}", file=sys.stderr, flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main()
