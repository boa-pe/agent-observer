#!/usr/bin/env python3
"""Strip a Copilot hook payload to the allowlist and POST it to the local viewer.

Reads the raw event JSON from stdin. Never forwards prompt, tool args, results,
error text, or full cwd — only event, tool name, repo directory name, session id.

Usage:
  ... | python viewer/hook_post.py --event preToolUse
  ... | python viewer/hook_post.py --event PreToolUse --url http://127.0.0.1:8770/api/hook
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request


DEFAULT_URL = "http://127.0.0.1:8770/api/hook"


def repo_name_from_cwd(cwd: str) -> str:
    p = os.path.abspath(cwd or os.getcwd())
    repo = os.path.basename(p.rstrip("\\/")) or "repo"
    q = p
    for _ in range(64):
        git = os.path.join(q, ".git")
        if os.path.isdir(git) or os.path.isfile(git):
            return os.path.basename(q.rstrip("\\/")) or repo
        n = os.path.dirname(q)
        if n == q:
            break
        q = n
    return repo


def build_payload(raw: dict, event: str) -> dict:
    cwd = raw.get("cwd") if isinstance(raw.get("cwd"), str) else ""
    tool = raw.get("toolName") or raw.get("tool_name") or raw.get("tool") or ""
    sid = raw.get("sessionId") or raw.get("session_id") or raw.get("session") or ""
    if not isinstance(tool, str):
        tool = ""
    if not isinstance(sid, str):
        sid = ""
    out = {"event": event, "repo": repo_name_from_cwd(cwd)}
    if tool:
        out["tool"] = tool
    if sid:
        out["session"] = sid
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--event", required=True, help="Hook event name to report")
    ap.add_argument("--url", default=DEFAULT_URL, help="POST URL (default: local viewer)")
    args = ap.parse_args()
    try:
        raw = json.loads(sys.stdin.read() or "{}")
    except json.JSONDecodeError:
        raw = {}
    if not isinstance(raw, dict):
        raw = {}
    body = json.dumps(build_payload(raw, args.event)).encode("utf-8")
    req = urllib.request.Request(
        args.url,
        data=body,
        headers={"Content-Type": "application/json", "Expect": ""},
        method="POST",
    )
    try:
        urllib.request.urlopen(req, timeout=2)
    except (urllib.error.URLError, TimeoutError, OSError):
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
