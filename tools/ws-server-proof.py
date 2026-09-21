#!/usr/bin/env python3
"""Runs ff-net.c against a real WebSocket server, built here out of a raw socket.

What this reaches that tests/test_ws_conn.c cannot: curl's CONNECT_ONLY socket, an actual TCP
connection, and bytes arriving in whatever sized pieces the kernel chooses. The unit test proves
the state machine; this proves the state machine is wired to a socket correctly.

The server is forty lines of `socket` and `hashlib` -- no websockets library, nothing to install,
nothing that reaches the network. It is also deliberately UNFORGIVING: it verifies the client's
handshake and that every client frame is masked, because a real server would simply hang up and
we would learn nothing.

    tools/ws-server-proof.py <path to ws_cli>
"""

import base64
import hashlib
import os
import socket
import struct
import subprocess
import sys
import threading

# RFC 6455 s1.3's GUID. Checked below against the RFC's own published example rather than typed
# from memory: the first version of this file had the last group as "5AB0DC85B11A", which is the
# same 36 characters rearranged, and every handshake failed -- against a client that was right.
# A proof's own reference implementation is not exempt from being verified.
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
RFC_KEY = "dGhlIHNhbXBsZSBub25jZQ=="
RFC_ACCEPT = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
FAILS = []
CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))


def frame(op, payload=b"", fin=True):
    """A SERVER frame: never masked (RFC 6455 s5.1)."""
    b0 = (0x80 if fin else 0) | op
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", b0, n)
    elif n < 65536:
        head = struct.pack("!BBH", b0, 126, n)
    else:
        head = struct.pack("!BBQ", b0, 127, n)
    return head + payload


def read_frame(sock, note):
    """Reads one CLIENT frame, insisting it is masked, and returns (opcode, payload)."""
    head = recv_exact(sock, 2)
    if head is None:
        return None, None
    b0, b1 = head
    op = b0 & 0x0F
    masked = bool(b1 & 0x80)
    note.append(("masked", masked))
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else b"\0\0\0\0"
    data = recv_exact(sock, n) or b""
    if masked:
        data = bytes(c ^ mask[i & 3] for i, c in enumerate(data))
    return op, data


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class Server(threading.Thread):
    """One connection, one script. `mode` picks which refusal to test."""

    def __init__(self, mode="ok"):
        super().__init__(daemon=True)
        self.mode = mode
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.saw = {}
        self.notes = []

    def run(self):
        try:
            self._serve()
        except Exception as e:  # a dead server must not hang the proof silently
            self.saw["error"] = repr(e)

    def _serve(self):
        conn, _ = self.sock.accept()
        conn.settimeout(10)
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        self.saw["request"] = req.decode("latin-1")

        key = ""
        for line in self.saw["request"].split("\r\n"):
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        self.saw["key"] = key
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()

        if self.mode == "http200":
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                         b"Content-Length: 6\r\n\r\n<html>")
            conn.close()
            return
        if self.mode == "badaccept":
            accept = "AAAAAAAAAAAAAAAAAAAAAAAAAAA="

        head = ("HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode()

        if self.mode == "badaccept":
            conn.sendall(head)
            conn.close()
            return

        if self.mode == "fin":
            # A clean TCP close with NO close frame -- a router dropping the connection, or a
            # server going away mid-deploy. curl reports this as OK with zero bytes, and a client
            # that read that as "nothing yet" would wait on a socket that will never speak again.
            conn.sendall(head)
            conn.shutdown(socket.SHUT_WR)
            conn.close()
            return

        # The 101 and the first frame in ONE write, deliberately: a client that clears its buffer
        # after the handshake loses this message and reports nothing at all.
        conn.sendall(head + frame(0x1, b"welcome"))

        # A message split across two frames, and across two writes, so reassembly is exercised
        # over a real socket rather than a scripted buffer.
        conn.sendall(frame(0x1, b"frag-", fin=False))
        conn.sendall(frame(0x0, b"mented"))

        # A ping that must be answered, sharing a write with a message that must not be delayed.
        conn.sendall(frame(0x9, b"areyouthere") + frame(0x1, b"after-ping"))

        # Whatever the client sends back, plus its pong, plus its close.
        for _ in range(4):
            op, data = read_frame(conn, self.notes)
            if op is None:
                break
            if op == 0xA:
                self.saw["pong"] = data
            elif op == 0x1:
                self.saw["client_text"] = data
            elif op == 0x8:
                self.saw["client_close"] = data
                break
            if "pong" in self.saw and "client_text" in self.saw:
                break

        conn.sendall(frame(0x8, struct.pack("!H", 1000)))
        try:
            read_frame(conn, self.notes)  # the client's close echo
        except Exception:
            pass
        conn.close()


def run_cli(cli, url, message=None):
    args = [cli, url] + ([message] if message else [])
    r = subprocess.run(args, capture_output=True, text=True, timeout=60)
    return r.stdout.strip().splitlines()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cli = sys.argv[1]

    # Before judging anything, prove this file can compute an accept value at all. Without it a
    # wrong GUID here reads as a broken client, which is the most expensive kind of false alarm.
    mine = base64.b64encode(hashlib.sha1(RFC_KEY.encode() + GUID).digest()).decode()
    check("this proof's own handshake maths matches RFC 6455's published example",
          mine == RFC_ACCEPT, f"computed {mine}, the RFC publishes {RFC_ACCEPT}")
    if mine != RFC_ACCEPT:
        print("\nws-server proof: the reference is wrong; every other result would be noise")
        return 1
    if not os.path.isfile(cli):
        print(f"ws-server-proof: {cli} is not built")
        return 2

    # ---- the whole conversation ----
    s = Server("ok")
    s.start()
    lines = run_cli(cli, f"ws://127.0.0.1:{s.port}/ws", "hello-from-client")
    s.join(timeout=15)
    print("  client said:", lines)

    check("the client connects and upgrades over a real socket", "OPEN" in lines)
    req = s.saw.get("request", "")
    check("its request asks for version 13 with a key",
          "Sec-WebSocket-Version: 13" in req and len(s.saw.get("key", "")) == 24,
          f"key={s.saw.get('key', '')!r}")
    check("it keeps a frame that shared a packet with the 101",
          "MSG:welcome" in lines,
          "a client that clears its buffer after the handshake loses this one silently")
    check("it reassembles a message split across two frames",
          "MSG:frag-mented" in lines)
    check("it answers a ping with the ping's own payload",
          s.saw.get("pong") == b"areyouthere", f"pong={s.saw.get('pong')!r}")
    check("a message sharing a packet with a ping is not delayed",
          "MSG:after-ping" in lines)
    check("everything it sent was masked",
          bool(s.notes) and all(v for k, v in s.notes if k == "masked"),
          f"{len(s.notes)} client frame(s) inspected")
    check("its own message arrives intact",
          s.saw.get("client_text") == b"hello-from-client",
          f"server received {s.saw.get('client_text')!r}")
    check("a close from the server ends it, with the code",
          any(l.startswith("END:") and "1000" in l for l in lines),
          next((l for l in lines if l.startswith("END:")), "no END line"))
    check("the server saw no error", "error" not in s.saw, s.saw.get("error", ""))

    # ---- a server that is not a WebSocket server ----
    s2 = Server("http200")
    s2.start()
    lines2 = run_cli(cli, f"ws://127.0.0.1:{s2.port}/ws")
    s2.join(timeout=15)
    check("a plain HTTP 200 is refused, not read as frames",
          any(l.startswith("REFUSED:") and "200" in l for l in lines2),
          next(iter(lines2), "no output"))

    # ---- a server whose accept value does not answer our key ----
    s3 = Server("badaccept")
    s3.start()
    lines3 = run_cli(cli, f"ws://127.0.0.1:{s3.port}/ws")
    s3.join(timeout=15)
    check("a wrong Sec-WebSocket-Accept is refused",
          any(l.startswith("REFUSED:") and "match" in l for l in lines3),
          next(iter(lines3), "no output"))

    # ---- a server that just hangs up, with no close frame ----
    s4 = Server("fin")
    s4.start()
    lines5 = run_cli(cli, f"ws://127.0.0.1:{s4.port}/ws")
    s4.join(timeout=15)
    check("a bare TCP close ends the connection instead of waiting forever",
          any(l.startswith("END:") for l in lines5),
          f"{lines5} -- curl reports a FIN as success with zero bytes, and reading that as "
          f"'nothing yet' hangs on a socket that will never speak again")

    # ---- a URL that is not one ----
    lines4 = run_cli(cli, "not-a-url")
    check("an unreadable URL is refused before any socket is opened",
          any(l.startswith("REFUSED:") for l in lines4), next(iter(lines4), "no output"))

    print(f"\nws-server proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("ws-server proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
