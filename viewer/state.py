"""In-memory agent wall: one card per repo, sessions aggregated by urgency."""

from __future__ import annotations

import threading
import time
from typing import Any

# waiting > error > working > idle > offline (same as agents_aggregate_state)
STATE_PRIO = {
    "offline": 0,
    "idle": 1,
    "working": 2,
    "error": 3,
    "waiting": 4,
}

QUESTION_TOOLS = frozenset({
    "askuserquestion",
    "exitplanmode",
    "ask_user",
    "askquestions",
    "ask_followup_question",
})

# Fields accepted from a stripped hook body (everything else is ignored).
ALLOW_KEYS = frozenset({
    "event",
    "hook_event_name",
    "hookEventName",
    "tool",
    "tool_name",
    "toolName",
    "repo",
    "session",
    "session_id",
    "sessionId",
})

DEFAULT_OFFLINE_TIMEOUT_MIN = 120


def sanitize_payload(raw: Any) -> dict[str, str]:
    """Keep only allowlisted string fields; drop prompt, toolArgs, paths, etc."""
    if not isinstance(raw, dict):
        return {}
    out: dict[str, str] = {}
    for k, v in raw.items():
        if k not in ALLOW_KEYS:
            continue
        if isinstance(v, str) and v:
            out[k] = v
    return out


def _pick(d: dict[str, str], *keys: str) -> str:
    for k in keys:
        if k in d and d[k]:
            return d[k]
    return ""


def map_event(event: str, tool: str = "") -> tuple[str | None, str | None, bool]:
    """Return (state, detail, keep). keep=True means touch last_seen only.

    state None means unknown / ignored event.
    """
    ev = (event or "").strip()
    if not ev:
        return None, None, False
    low = ev.lower()
    tool_l = (tool or "").lower()

    if low == "sessionstart":
        return "idle", "session started", False
    if low == "sessionend":
        return "offline", "session ended", False
    if low in ("userpromptsubmit", "userpromptsubmitted"):
        return "working", "thinking", False
    if low == "pretooluse":
        if tool_l in QUESTION_TOOLS:
            return "waiting", "question for you", False
        return "working", tool or None, False
    if low in ("posttooluse", "posttoolusefailure", "precompact", "postcompact"):
        return "working", None, False
    if low == "permissiondenied":
        return "working", f"denied {tool or 'tool'}", False
    if low in ("subagentstart", "subagentstop"):
        return None, None, True
    if low == "permissionrequest":
        return "waiting", f"allow {tool or 'tool'}?", False
    if low == "notification":
        # Without message text (stripped), treat as waiting for attention.
        return "waiting", "needs attention", False
    if low in ("stop", "agentstop"):
        return "idle", "done", False
    if low in ("erroroccurred", "stopfailure"):
        return "error", "error", False
    if low in STATE_PRIO:
        return low, None, False
    return None, None, False


def aggregate_state(states: list[str]) -> str:
    best = "offline"
    for s in states:
        if STATE_PRIO.get(s, 0) > STATE_PRIO.get(best, 0):
            best = s
    return best


class AgentWall:
    def __init__(self, offline_timeout_min: int = DEFAULT_OFFLINE_TIMEOUT_MIN):
        self._lock = threading.Lock()
        self._repos: dict[str, dict[str, Any]] = {}
        self.offline_timeout_min = max(0, int(offline_timeout_min))

    def handle_hook(self, raw: Any) -> bool:
        """Apply a sanitized hook payload. Returns True if a known repo was updated."""
        data = sanitize_payload(raw)
        event = _pick(data, "event", "hook_event_name", "hookEventName")
        tool = _pick(data, "tool", "tool_name", "toolName")
        repo = _pick(data, "repo")
        session = _pick(data, "session", "session_id", "sessionId") or "_"

        if not repo or not event:
            return False

        state, detail, keep = map_event(event, tool)
        if state is None and not keep:
            return False

        now = time.time()
        with self._lock:
            card = self._repos.get(repo)
            if card is None:
                card = {
                    "repo": repo,
                    "sessions": {},
                    "detail": "",
                    "last_event_at": now,
                }
                self._repos[repo] = card

            sess = card["sessions"].get(session)
            if sess is None:
                sess = {"state": "offline", "last_event_at": now}
                card["sessions"][session] = sess

            sess["last_event_at"] = now
            card["last_event_at"] = now

            if keep:
                return True

            if state == "offline":
                # Session ended: drop it. Card vanishes when no sessions remain.
                card["sessions"].pop(session, None)
                if not card["sessions"]:
                    self._repos.pop(repo, None)
                return True

            sess["state"] = state
            if detail:
                card["detail"] = detail[:40]
            elif tool and state == "working":
                card["detail"] = tool[:40]
            return True

    def tick(self, now: float | None = None) -> None:
        """Drop stale sessions / empty cards after offline timeout (0 = never)."""
        if self.offline_timeout_min <= 0:
            return
        now = time.time() if now is None else now
        limit = self.offline_timeout_min * 60.0
        with self._lock:
            dead_repos: list[str] = []
            for repo, card in self._repos.items():
                dead_sess: list[str] = []
                for sid, sess in card["sessions"].items():
                    if now - sess["last_event_at"] >= limit:
                        dead_sess.append(sid)
                for sid in dead_sess:
                    card["sessions"].pop(sid, None)
                if not card["sessions"]:
                    dead_repos.append(repo)
            for repo in dead_repos:
                self._repos.pop(repo, None)

    def list_agents(self) -> list[dict[str, Any]]:
        self.tick()
        with self._lock:
            out: list[dict[str, Any]] = []
            for repo, card in self._repos.items():
                states = [s["state"] for s in card["sessions"].values()]
                if not states:
                    continue
                out.append({
                    "repo": repo,
                    "state": aggregate_state(states),
                    "detail": card.get("detail") or "",
                    "sessions": len(card["sessions"]),
                    "kind": "copilot",
                })
            out.sort(key=lambda a: (-STATE_PRIO.get(a["state"], 0), a["repo"].lower()))
            return out
