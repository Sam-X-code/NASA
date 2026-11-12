import json
import os
import tempfile
import unittest

from agent import (
    Agent,
    JournalCorruptionError,
    RecoveryManager,
    TaskJournal,
    _stable_effect_key,
)


class CrashAfterCommitTools:
    """Simulates a guest commit followed by host death before acknowledgement."""
    def __init__(self, committed_keys=None, state=None, crash_once=False):
        self.committed_keys = committed_keys if committed_keys is not None else set()
        self.state = state if state is not None else {"append_count": 0}
        self.crash_once = crash_once
        self.seen_keys = []

    def execute(self, action, effect_key=None):
        self.seen_keys.append(effect_key)
        if effect_key not in self.committed_keys:
            self.committed_keys.add(effect_key)
            self.state["append_count"] += 1
        if self.crash_once:
            self.crash_once = False
            raise SystemExit("simulated Python process crash after guest commit")
        return {"ok": True, "output": "committed"}


class OneActionPlanner:
    def __init__(self):
        self.calls = 0

    def next_action(self, request, history):
        self.calls += 1
        if not history:
            return {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "B"}}
        return {"tool": "finish", "args": {"answer": "done"}}


class JournalRecoveryTests(unittest.TestCase):
    def test_effect_key_is_nonzero_uint64_and_deterministic(self):
        action = {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "B"}}
        first = _stable_effect_key("task-1", 0, action)
        self.assertEqual(first, _stable_effect_key("task-1", 0, action))
        self.assertGreater(first, 0)
        self.assertLessEqual(first, (1 << 64) - 1)
        self.assertNotEqual(first, _stable_effect_key("task-1", 1, action))

    def test_action_and_key_are_replayed_after_crash_after_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            journal_path = os.path.join(directory, "tasks.jsonl")
            trace_path = os.path.join(directory, "trace.jsonl")
            committed = set()
            state = {"append_count": 0}
            first_tools = CrashAfterCommitTools(committed, state, crash_once=True)
            first_planner = OneActionPlanner()
            first_agent = Agent(first_planner, first_tools, log_path=trace_path,
                                max_steps=3, journal_path=journal_path)

            with self.assertRaises(SystemExit):
                first_agent.run("perform the operation")

            journal = TaskJournal(journal_path)
            pending = journal.recover_pending()
            self.assertIsNotNone(pending)
            step = pending["steps"][0]
            self.assertEqual(step["status"], "RUNNING")
            original_action = step["action"]
            original_key = step["effect_key"]
            self.assertIsInstance(original_key, int)
            self.assertNotEqual(original_key, 0)
            self.assertEqual(state["append_count"], 1)

            second_tools = CrashAfterCommitTools(committed, state)
            second_planner = OneActionPlanner()

            def crash_before_completion(_payload):
                raise SystemExit("restart before completion persistence")

            second_agent = Agent(second_planner, second_tools, log_path=trace_path,
                                 max_steps=3, journal_path=journal_path,
                                 crash_hook=crash_before_completion)
            with self.assertRaises(SystemExit):
                second_agent.run("perform the operation")

            self.assertEqual(second_tools.seen_keys[0], original_key)
            self.assertEqual(state["append_count"], 1,
                             "replay on the second restart must not repeat the mutation")
            self.assertEqual(second_planner.calls, 0,
                             "a persisted operation must not be regenerated")

            third_tools = CrashAfterCommitTools(committed, state)
            third_agent = Agent(OneActionPlanner(), third_tools, log_path=trace_path,
                                max_steps=3, journal_path=journal_path)
            result = third_agent.run("perform the operation")
            self.assertEqual(result, "done")
            self.assertEqual(third_tools.seen_keys[0], original_key)
            self.assertEqual(state["append_count"], 1,
                             "repeated same-key recovery must remain idempotent")
            final_task = journal.get_task(pending["task_id"])
            self.assertEqual(final_task["steps"][0]["action"], original_action)
            self.assertEqual(final_task["steps"][0]["status"], "COMPLETED")
            self.assertEqual(final_task["status"], "COMPLETED")

    def test_agent_crash_hook_triggers_after_success_before_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            journal_path = os.path.join(directory, "tasks.jsonl")
            trace_path = os.path.join(directory, "trace.jsonl")

            class SuccessTools:
                def execute(self, action, effect_key=None):
                    return {"ok": True, "output": "committed"}

            seen = {"called": False}
            def crash_hook(payload):
                seen["called"] = True
                self.assertEqual(payload["action"]["tool"], "append_file")
                self.assertIn("effect_key", payload)
                raise SystemExit(91)

            agent = Agent(OneActionPlanner(), SuccessTools(), log_path=trace_path,
                          max_steps=3, journal_path=journal_path, crash_hook=crash_hook)
            with self.assertRaises(SystemExit):
                agent.run("perform the operation")
            self.assertTrue(seen["called"])

    def test_bridge_exception_keeps_step_pending_for_safe_replay(self):
        class ErrorAfterCommitTools(CrashAfterCommitTools):
            def execute(self, action, effect_key=None):
                self.seen_keys.append(effect_key)
                if effect_key not in self.committed_keys:
                    self.committed_keys.add(effect_key)
                    self.state["append_count"] += 1
                if self.crash_once:
                    self.crash_once = False
                    raise RuntimeError("serial bridge disconnected after command")
                return {"ok": True, "output": "committed"}

        with tempfile.TemporaryDirectory() as directory:
            journal_path = os.path.join(directory, "tasks.jsonl")
            trace_path = os.path.join(directory, "trace.jsonl")
            committed, state = set(), {"append_count": 0}
            first = Agent(OneActionPlanner(),
                          ErrorAfterCommitTools(committed, state, crash_once=True),
                          log_path=trace_path, max_steps=3, journal_path=journal_path)
            output = first.run("perform the operation")
            self.assertIn("outcome uncertain", output)
            pending = TaskJournal(journal_path).recover_pending()
            self.assertIsNotNone(pending)
            self.assertEqual(pending["steps"][0]["status"], "RUNNING")
            key = pending["steps"][0]["effect_key"]

            second_tools = ErrorAfterCommitTools(committed, state)
            result = Agent(OneActionPlanner(), second_tools, log_path=trace_path,
                           max_steps=3, journal_path=journal_path).run("perform the operation")
            self.assertEqual(result, "done")
            self.assertEqual(second_tools.seen_keys[0], key)
            self.assertEqual(state["append_count"], 1)

    def test_torn_final_record_is_discarded_but_complete_records_survive(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            journal = TaskJournal(path)
            task = journal.create_task("hello")
            with open(path, "ab") as handle:
                handle.write(b'{"task_id":"half-written"')
            recovered = journal.get_task(task["task_id"])
            self.assertEqual(recovered["request"], "hello")
            with open(path, "rb") as handle:
                self.assertTrue(handle.read().endswith(b"\n"))

    def test_complete_unterminated_pending_record_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            journal = TaskJournal(path)
            task = journal.create_task("preserve complete tail")
            journal.update_task(task["task_id"], status="RUNNING")
            with open(path, "rb") as handle:
                records = [json.loads(line) for line in handle if line.endswith(b"\n")]
            with open(path, "wb") as handle:
                handle.write(json.dumps(records[-1]).encode("utf-8"))

            pending = journal.recover_pending()
            self.assertIsNotNone(pending)
            self.assertEqual(pending["task_id"], task["task_id"])
            with open(path, "rb") as handle:
                self.assertTrue(handle.read().endswith(b"\n"))

    def test_complete_unterminated_invalid_record_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            with open(path, "wb") as handle:
                handle.write(b"null")
            with self.assertRaises(JournalCorruptionError):
                TaskJournal(path).recover_pending()

    def test_workspace_mismatch_blocks_pending_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            first = TaskJournal(path, workspace_id="workspace-a")
            task = first.create_task("workspace-scoped")
            first.update_task(task["task_id"], status="RUNNING")

            with self.assertRaises(JournalCorruptionError):
                TaskJournal(path, workspace_id="workspace-b").recover_pending()

    def test_recovery_starts_after_all_completed_steps(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = TaskJournal(os.path.join(directory, "tasks.jsonl"))
            task = journal.create_task("continue after completed step")
            journal.mark_step(
                task["task_id"], 0, "COMPLETED",
                action={"tool": "read_file", "args": {"path": "/report.txt"}},
                result={"ok": True, "output": "read"},
            )
            journal.update_task(task["task_id"], status="RUNNING")
            self.assertEqual(RecoveryManager(journal).recover()["resume_from"], 1)

    def test_compaction_preserves_latest_task_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            journal = TaskJournal(path)
            task = journal.create_task("compact me")
            for index in range(8):
                journal.update_task(task["task_id"], current_step=index)
            before = os.path.getsize(path)
            journal.compact()
            after = os.path.getsize(path)
            recovered = journal.get_task(task["task_id"])
            self.assertEqual(recovered["current_step"], 7)
            self.assertLess(after, before)

    def test_step_action_cannot_be_replaced_after_persistence(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = TaskJournal(os.path.join(directory, "tasks.jsonl"))
            task = journal.create_task("immutable action")
            action = {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "A"}}
            journal.mark_step(task["task_id"], 0, "PREPARED", action=action,
                              effect_key=17)
            with self.assertRaises(ValueError):
                journal.mark_step(task["task_id"], 0, "PREPARED",
                                  action={"tool": "append_file", "args": {
                                      "path": "/agentlog.txt", "data": "B"}},
                                  effect_key=17)

    def test_corrupt_complete_record_is_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "tasks.jsonl")
            with open(path, "wb") as handle:
                handle.write(b'{not-json}\n')
            with self.assertRaises(JournalCorruptionError):
                TaskJournal(path).get_task("missing")


if __name__ == "__main__":
    unittest.main()
