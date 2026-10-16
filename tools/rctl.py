#!/usr/bin/env python3
"""rctl - command line client for the sdrouter control plane.

Examples:
    ./rctl.py show routes
    ./rctl.py add-route 30.0.0.0/24 rt3
    ./rctl.py add-route 0.0.0.0/0 rt1 --via 10.0.1.1
    ./rctl.py del-route 30.0.0.0/24
    ./rctl.py add-neigh 10.0.2.10 aa:bb:cc:dd:ee:ff
    ./rctl.py raw "SHOW STATS"
    ./rctl.py repl
"""
import argparse
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5555


class Router:
    """One connection to the router's control plane.

    The protocol is line in, block out, terminated by a line starting with
    'OK' or 'ERR'. That framing lets us read a reply of unknown length without
    a length prefix or a timeout guess.
    """

    def __init__(self, host, port, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self._read_reply()          # consume the greeting banner

    def _read_reply(self):
        lines = []
        while True:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise ConnectionError("router closed the connection")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace")
            lines.append(text)
            if text.startswith("OK") or text.startswith("ERR"):
                return "\n".join(lines)

    def send(self, command):
        self.sock.sendall(command.encode() + b"\n")
        return self._read_reply()

    def close(self):
        try:
            self.sock.sendall(b"QUIT\n")
        except OSError:
            pass
        self.sock.close()


def build_command(args):
    if args.cmd == "show":
        return "SHOW " + args.what.upper()
    if args.cmd == "add-route":
        cmd = f"ADD_ROUTE {args.prefix} {args.iface}"
        return cmd + (f" VIA {args.via}" if args.via else "")
    if args.cmd == "del-route":
        return f"DEL_ROUTE {args.prefix}"
    if args.cmd == "add-neigh":
        return f"ADD_NEIGH {args.ip} {args.mac}"
    if args.cmd == "del-neigh":
        return f"DEL_NEIGH {args.ip}"
    if args.cmd == "ping":
        return "PING"
    if args.cmd == "raw":
        return args.text
    raise ValueError(args.cmd)


def repl(router):
    print("connected. type HELP for commands, QUIT to exit.")
    while True:
        try:
            line = input("sdrouter> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not line:
            continue
        try:
            print(router.send(line), end="\n")
        except ConnectionError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        if line.upper() in ("QUIT", "EXIT"):
            return 0


def main():
    ap = argparse.ArgumentParser(description="sdrouter control plane client")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("show", help="show routes|neigh|ifaces|stats")
    p.add_argument("what", choices=["routes", "neigh", "ifaces", "stats"])

    p = sub.add_parser("add-route")
    p.add_argument("prefix", help="a.b.c.d/len")
    p.add_argument("iface")
    p.add_argument("--via", help="next hop IP")

    p = sub.add_parser("del-route")
    p.add_argument("prefix")

    p = sub.add_parser("add-neigh")
    p.add_argument("ip")
    p.add_argument("mac")

    p = sub.add_parser("del-neigh")
    p.add_argument("ip")

    sub.add_parser("ping", help="liveness check")
    sub.add_parser("repl", help="interactive session")

    p = sub.add_parser("raw")
    p.add_argument("text", help="a literal protocol line")

    args = ap.parse_args()

    try:
        router = Router(args.host, args.port)
    except OSError as exc:
        print(f"cannot reach router at {args.host}:{args.port}: {exc}",
              file=sys.stderr)
        return 1

    try:
        if args.cmd == "repl":
            return repl(router)
        reply = router.send(build_command(args))
        print(reply)
        return 1 if reply.splitlines()[-1].startswith("ERR") else 0
    finally:
        router.close()


if __name__ == "__main__":
    sys.exit(main())
