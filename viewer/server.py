#!/usr/bin/env python3
"""Local agent wall — same hook URL shape as the device, on 127.0.0.1:8770."""

from __future__ import annotations

import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# Allow `python viewer/server.py` from the repo root.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from state import AgentWall  # noqa: E402

HOST = "127.0.0.1"
PORT = 8770
STATIC = _HERE / "static"
WALL = AgentWall()

# Same path as the device: /api/hook/<id>
_HOOK_PATH = re.compile(r"^/api/hook/([^/]+)/?$")


class Handler(BaseHTTPRequestHandler):
    server_version = "AgentObserverViewer/1.0"

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _cors(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self._cors()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _empty(self, code: int = 200) -> None:
        self.send_response(code)
        self._cors()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _file(self, path: Path, content_type: str) -> None:
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self._cors()
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self._cors()
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/api/agents":
            self._json(200, {"agents": WALL.list_agents()})
            return
        if path in ("/", "/index.html"):
            self._file(STATIC / "index.html", "text/html; charset=utf-8")
            return
        if path.startswith("/static/"):
            rel = path[len("/static/") :]
            if ".." in rel or rel.startswith(("/", "\\")):
                self._empty(404)
                return
            f = STATIC / rel
            if f.is_file():
                ctype = "application/octet-stream"
                if rel.endswith(".css"):
                    ctype = "text/css; charset=utf-8"
                elif rel.endswith(".js"):
                    ctype = "application/javascript; charset=utf-8"
                elif rel.endswith(".html"):
                    ctype = "text/html; charset=utf-8"
                elif rel.endswith(".svg"):
                    ctype = "image/svg+xml"
                self._file(f, ctype)
                return
        self._empty(404)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        m = _HOOK_PATH.match(parsed.path)
        if not m:
            self._empty(404)
            return
        agent_id = m.group(1)
        qs = parse_qs(parsed.query)
        event = (qs.get("event") or [""])[0]
        tool = (qs.get("tool") or [""])[0]
        ntype = (qs.get("ntype") or [""])[0]

        # Optional JSON body (older hooks / scripts) — never required by generated examples.
        length = int(self.headers.get("Content-Length") or 0)
        raw_bytes = self.rfile.read(min(length, 4096)) if length else b""
        if raw_bytes:
            try:
                raw = json.loads(raw_bytes.decode("utf-8") or "{}")
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._empty(400)
                return
            if isinstance(raw, dict):
                if not event:
                    event = (
                        raw.get("hook_event_name")
                        or raw.get("hookEventName")
                        or raw.get("event")
                        or ""
                    )
                if not tool:
                    tool = raw.get("tool_name") or raw.get("toolName") or raw.get("tool") or ""
                if not ntype:
                    ntype = (
                        raw.get("notification_type")
                        or raw.get("notificationType")
                        or raw.get("ntype")
                        or ""
                    )

        if not isinstance(event, str):
            event = ""
        if not isinstance(tool, str):
            tool = ""
        if not isinstance(ntype, str):
            ntype = ""

        WALL.handle_hook(agent_id, event=event, tool=tool, ntype=ntype)
        # Empty 200 so command hooks never treat the body as hook output.
        self._empty(200)


def main() -> None:
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Agent Observer viewer on http://{HOST}:{PORT}/", flush=True)
    print("POST device-style hooks to /api/hook/<id>?event=…  |  GET /api/agents", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.", flush=True)


if __name__ == "__main__":
    os.chdir(_HERE)
    main()
