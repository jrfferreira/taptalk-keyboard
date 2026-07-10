#!/usr/bin/env python3
"""Serve the device's setup page on a laptop, so it can be reviewed and tested
without flashing a board.

The page lives as a C string literal in main/provisioning.c. This extracts it,
decodes the C escapes, and serves it with a mock /scan that includes the kind
of SSIDs a hostile neighbour would broadcast.

    python3 tools/preview_portal.py       # http://127.0.0.1:4174
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "main" / "provisioning.c"
PORT = 4174

# What the device would send after json_escape(). The first two are the reason
# the page must use textContent and never innerHTML.
MOCK_SCAN = [
    {"s": '<img src=x onerror="window.__xss=1">', "p": 1},
    {"s": 'quote"net\\path', "p": 0},
    {"s": "Café ☕", "p": 1},
    {"s": "HomeWiFi-5G", "p": 1},
    {"s": "openguest", "p": 0},
]


def decode_c_escapes(s: str) -> bytes:
    out, i = bytearray(), 0
    simple = {"n": 10, "t": 9, "r": 13, '"': 34, "\\": 92, "'": 39, "0": 0}
    while i < len(s):
        c = s[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        nxt = s[i + 1]
        if nxt == "x":
            out.append(int(s[i + 2:i + 4], 16))
            i += 4
        elif nxt in simple:
            out.append(simple[nxt])
            i += 2
        else:
            # e.g. … inside the JS: C leaves the backslash for JS to read.
            out += c.encode()
            i += 1
    return bytes(out)


def extract(name: str) -> str:
    src = SRC.read_text()
    m = re.search(rf"static const char {name}\[\]\s*=(.*?);\s*\n", src, re.S)
    if not m:
        sys.exit(f"could not find {name} in {SRC}")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)  # drop C comments
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if not parts:
        sys.exit(f"{name} has no string literals")
    return b"".join(decode_c_escapes(p) for p in parts).decode("utf-8")


class Handler(BaseHTTPRequestHandler):
    def _send(self, body: bytes, ctype: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/scan"):
            self._send(json.dumps(MOCK_SCAN).encode(), "application/json")
        else:
            self._send(extract("PAGE_FORM").encode(), "text/html; charset=utf-8")

    def do_POST(self):
        self._send(extract("PAGE_SAVED").encode(), "text/html; charset=utf-8")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print(f"setup portal on http://127.0.0.1:{PORT}  (mock scan, {len(MOCK_SCAN)} networks)")
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
