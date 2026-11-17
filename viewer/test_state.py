"""Unit tests for the agent wall state machine (no network)."""

from __future__ import annotations

import time
import unittest

from state import AgentWall, map_event


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

    def test_notification_ntypes(self):
        self.assertEqual(map_event("Notification", ntype="idle_prompt")[0], "idle")
        self.assertEqual(map_event("Notification", ntype="auth_success")[2], True)
        st, detail, _ = map_event("Notification", ntype="permission_prompt")
        self.assertEqual(st, "waiting")
        self.assertEqual(detail, "needs permission")
        st, detail, _ = map_event("Notification")
        self.assertEqual(st, "waiting")
        self.assertEqual(detail, "needs attention")


class WallTests(unittest.TestCase):
    def test_one_card_per_agent_id(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook("backend", "sessionStart")
        w.handle_hook("backend", "preToolUse", tool="edit")
        agents = w.list_agents()
        self.assertEqual(len(agents), 1)
        self.assertEqual(agents[0]["id"], "backend")
        self.assertEqual(agents[0]["state"], "working")
        self.assertEqual(agents[0]["detail"], "edit")

    def test_session_end_marks_offline(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook("web", "sessionStart")
        w.handle_hook("web", "sessionEnd")
        agents = w.list_agents()
        self.assertEqual(len(agents), 1)
        self.assertEqual(agents[0]["state"], "offline")

    def test_subagent_does_not_change_state(self):
        w = AgentWall(offline_timeout_min=0)
        w.handle_hook("x", "sessionStart")
        w.handle_hook("x", "Stop")
        w.handle_hook("x", "subagentStart")
        self.assertEqual(w.list_agents()[0]["state"], "idle")

    def test_rejects_empty(self):
        w = AgentWall()
        self.assertFalse(w.handle_hook("", "sessionStart"))
        self.assertFalse(w.handle_hook("a", ""))
        self.assertEqual(w.list_agents(), [])

    def test_stale_timeout(self):
        w = AgentWall(offline_timeout_min=1)
        w.handle_hook("old", "sessionStart")
        with w._lock:
            w._agents["old"]["last_event_at"] = time.time() - 120
        agents = w.list_agents()
        self.assertEqual(agents[0]["state"], "offline")


if __name__ == "__main__":
    unittest.main()
