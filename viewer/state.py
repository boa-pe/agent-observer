"""In-memory agent wall — same hook semantics as main/agents.c (one card per agent id)."""

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

DEFAULT_OFFLINE_TIMEOUT_MIN = 120


def map_event(
    event: str, tool: str = "", ntype: str = ""
) -> tuple[str | None, str | None, bool]:
    """Return (state, detail, keep). keep=True means touch last_seen only.

    Mirrors agents_handle_hook in main/agents.c (bodyless hooks from origin/main).
    """
    ev = (event or "").strip()
    if not ev:
        return None, None, False
    low = ev.lower()
    tool_l = (tool or "").lower()
    ntype_l = (ntype or "").lower()

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
        if ntype_l == "idle_prompt":
            return "idle", None, False
        if ntype_l == "auth_success":
            return None, None, True
        if ntype_l == "permission_prompt":
            return "waiting", "needs permission", False
        if ntype_l == "elicitation_dialog":
            return "waiting", "input needed", False
        return "waiting", "needs attention", False
    if low in ("stop", "agentstop"):
        return "idle", "done", False
    if low in ("erroroccurred", "stopfailure"):
        return "error", "error", False
    if low in STATE_PRIO:
        return low, None, False
    return None, None, False


class AgentWall:
    """One robot per agent id — same model as the LCD slots."""

    def __init__(self, offline_timeout_min: int = DEFAULT_OFFLINE_TIMEOUT_MIN):
        self._lock = threading.Lock()
        self._agents: dict[str, dict[str, Any]] = {}
        self.offline_timeout_min = max(0, int(offline_timeout_min))

    def handle_hook(
        self,
        agent_id: str,
        event: str = "",
        tool: str = "",
        ntype: str = "",
    ) -> bool:
        """Apply a device-style hook. Auto-creates the slot on first event (no web UI)."""
        agent_id = (agent_id or "").strip()
        if not agent_id or not event:
            return False

        state, detail, keep = map_event(event, tool, ntype)
        if state is None and not keep:
            return False

        now = time.time()
        with self._lock:
            card = self._agents.get(agent_id)
            if card is None:
                card = {
                    "id": agent_id,
                    "name": agent_id,
                    "state": "offline",
                    "detail": "",
                    "kind": "other",
                    "last_event_at": now,
                    "since": now,
                }
                self._agents[agent_id] = card

            card["last_event_at"] = now
            if keep:
                return True

            if card["state"] != state:
                card["since"] = now
            card["state"] = state
            if detail:
                card["detail"] = detail[:40]
            elif tool and state == "working":
                card["detail"] = tool[:40]
            elif state == "offline":
                card["detail"] = detail or "session ended"
            return True

    def tick(self, now: float | None = None) -> None:
        """Mark stale agents offline (same idea as the device timeout)."""
        if self.offline_timeout_min <= 0:
            return
        now = time.time() if now is None else now
        limit = self.offline_timeout_min * 60.0
        with self._lock:
            for card in self._agents.values():
                if card["state"] == "offline":
                    continue
                if now - card["last_event_at"] >= limit:
                    if card["state"] != "offline":
                        card["since"] = now
                    card["state"] = "offline"
                    card["detail"] = "stale"

    def list_agents(self) -> list[dict[str, Any]]:
        self.tick()
        with self._lock:
            out = [
                {
                    "id": c["id"],
                    "name": c["name"],
                    "state": c["state"],
                    "detail": c.get("detail") or "",
                    "kind": c.get("kind") or "other",
                    "since": c.get("since") or c["last_event_at"],
                }
                for c in self._agents.values()
            ]
            out.sort(key=lambda a: (-STATE_PRIO.get(a["state"], 0), a["name"].lower()))
            return out
