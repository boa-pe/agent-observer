"""Unit tests for the agent wall state machine (no network)."""

from __future__ import annotations

import time
import unittest

from state import AgentWall, aggregate_state, map_event, sanitize_payload


class SanitizeTests(unittest.TestCase):
    def test_drops_sensitive_fields(self):
        raw = {
            "event": "preToolUse",
            "toolName": "edit",
            "repo": "backend",
            "sessionId": "abc123",
            "prompt": "secret prompt",
            "toolArgs": {"path": "/secret"},
            "toolResult": {"textResultForLlm": "code"},
            "error": "stack trace",
            "cwd": "C:/Users/me/Work/backend",
            "message": "please approve",
        }
        clean = sanitize_payload(raw)
        self.assertEqual(clean["event"], "preToolUse")
        self.assertEqual(clean["toolName"], "edit")
        self.assertEqual(clean["repo"], "backend")
        self.assertEqual(clean["sessionId"], "abc123")
        for bad in ("prompt", "toolArgs", "toolResult", "error", "cwd", "message"):
            self.assertNotIn(bad, clean)


class MapEventTests(unittest.TestCase):
    def test_session_and_prompt(self):
        self.assertEqual(map_event("sessionStart")[0], "idle")
        self.assertEqual(map_event("UserPromptSubmit")[0], "working")
        self.assertEqual(map_event("userPromptSubmitted")[1], "thinking")

    def test_question_tool_waiting(self):
        st, detail, keep = map_event("PreToolUse", "AskUserQuestion")
        self.assertEqual(st, "waiting")
        self.assertFalse(keep)

    def test_subagent_keep(self):
        st, detail, keep = map_event("subagentStart")
        self.assertIsNone(st)
        self.assertTrue(keep)

    def test_error_literal(self):
        st, detail, _ = map_event("errorOccurred")
        self.assertEqual(st, "error")
        self.assertEqual(detail, "error")

    def test_notification_without_message(self):
        st, detail, _ = map_event("Notification")
        self.assertEqual(st, "waiting")
        self.assertEqual(detail, "needs attention")


class AggregateTests(unittest.TestCase):
    def test_priority(self):
        self.assertEqual(aggregate_state(["idle", "working"]), "working")
        self.assertEqual(aggregate_state(["working", "waiting", "error"]), "waiting")
        self.assertEqual(aggregate_state(["offline"]), "offline")


class WallTests(unittest.TestCase):
    def test_one_card_per_repo_two_sessions(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook({"event": "sessionStart", "repo": "api", "sessionId": "s1"})
        w.handle_hook({"event": "sessionStart", "repo": "api", "sessionId": "s2"})
        w.handle_hook({"event": "preToolUse", "repo": "api", "sessionId": "s1", "toolName": "edit"})
        agents = w.list_agents()
        self.assertEqual(len(agents), 1)
        self.assertEqual(agents[0]["repo"], "api")
        self.assertEqual(agents[0]["sessions"], 2)
        self.assertEqual(agents[0]["state"], "working")
        self.assertEqual(agents[0]["detail"], "edit")

    def test_session_end_removes_card(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook({"event": "sessionStart", "repo": "web", "session": "a"})
        w.handle_hook({"event": "sessionEnd", "repo": "web", "session": "a"})
        self.assertEqual(w.list_agents(), [])

    def test_subagent_does_not_change_state(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook({"event": "sessionStart", "repo": "x", "sessionId": "1"})
        w.handle_hook({"event": "Stop", "repo": "x", "sessionId": "1"})
        w.handle_hook({"event": "subagentStart", "repo": "x", "sessionId": "1"})
        agents = w.list_agents()
        self.assertEqual(agents[0]["state"], "idle")

    def test_rejects_raw_sensitive_only_payload(self):
        w = AgentWall()
        ok = w.handle_hook({"prompt": "hi", "cwd": "/tmp/proj"})
        self.assertFalse(ok)
        self.assertEqual(w.list_agents(), [])

    def test_stale_timeout(self):
        w = AgentWall(offline_timeout_min=1)  # 1 minute
        w.handle_hook({"event": "sessionStart", "repo": "old", "sessionId": "1"})
        # Force last_event far in the past
        with w._lock:
            for sess in w._repos["old"]["sessions"].values():
                sess["last_event_at"] = time.time() - 120
        self.assertEqual(w.list_agents(), [])


if __name__ == "__main__":
    unittest.main()
