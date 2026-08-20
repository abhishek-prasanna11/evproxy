#!/usr/bin/env python3
"""Open N connections to the proxy and hold them open without sending anything.

Established-but-idle connections are what separate the architectures. In the thread pool each one
occupies a worker; in the event loop each one is a few hundred bytes the loop never touches. They
are also how you catch a spinning loop: an idle proxy must burn no CPU, and a permanently-armed
EVFILT_WRITE would show up here as 100% CPU while every correctness test still passes.
"""

import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--hold", type=float, default=3.0)
    args = ap.parse_args()

    socks = []
    for _ in range(args.count):
        try:
            s = socket.create_connection((args.host, args.port), timeout=5)
            socks.append(s)
        except OSError as e:
            print(f"connected {len(socks)} before failing: {e}", file=sys.stderr)
            break

    print(len(socks), flush=True)
    time.sleep(args.hold)

    for s in socks:
        try:
            s.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()
