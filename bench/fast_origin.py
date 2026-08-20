#!/usr/bin/env python3
"""A deliberately minimal origin: pre-baked response, no routing, no file I/O, no parsing beyond
finding the end of the request headers.

`python3 -m http.server` saturated at ~2.4k req/s, which is BELOW what the proxy under test can do --
so the first benchmark measured the origin, and all three proxy arms flatlined at the same number.
An origin has to be comfortably faster than the thing being benchmarked or it is the thing being
benchmarked.

Runs K processes over one SO_REUSEPORT socket so the origin scales past a single asyncio loop.
"""

import argparse
import asyncio
import os
import socket
import sys

RESPONSE = None


async def handle(reader, writer):
    try:
        while True:
            # Read until end of headers. The proxy sends Connection: close and one request per
            # connection, so this loop normally runs once.
            data = await reader.readuntil(b"\r\n\r\n")
            if not data:
                break
            writer.write(RESPONSE)
            await writer.drain()
            break
    except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError,
            asyncio.LimitOverrunError):
        pass
    finally:
        try:
            writer.close()
        except OSError:
            pass


def make_socket(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind((host, port))
    s.listen(4096)
    s.setblocking(False)
    return s


async def serve(sock):
    server = await asyncio.start_server(handle, sock=sock, backlog=4096)
    async with server:
        await server.serve_forever()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--size", type=int, default=8192)
    ap.add_argument("--procs", type=int, default=4)
    args = ap.parse_args()

    global RESPONSE
    body = b"x" * args.size
    RESPONSE = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/octet-stream\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n"
        b"Connection: close\r\n"
        b"\r\n" + body
    )

    for _ in range(args.procs - 1):
        if os.fork() == 0:
            break

    sock = make_socket(args.host, args.port)
    print(f"fast origin pid={os.getpid()} on {args.host}:{args.port}", file=sys.stderr, flush=True)
    try:
        asyncio.run(serve(sock))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
