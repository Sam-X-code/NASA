import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path

from agent import (
    TaskJournal,
    default_journal_path,
    workspace_identifier,
)
from bridge import MockBridge
from main import run_application


class RecordingBridge(MockBridge):
    def __init__(self):
        super().__init__()
        self.calls = []

    def run(self, tool, args, effect_key=None):
        self.calls.append((tool, args.copy(), effect_key))
        return super().run(tool, args, effect_key=effect_key)


class MainRecoveryTests(unittest.TestCase):
    def test_default_journal_path_is_stable_and_workspace_scoped(self):
        with tempfile.TemporaryDirectory() as state_home:
            with tempfile.TemporaryDirectory() as first_workspace, \
                    tempfile.TemporaryDirectory() as second_workspace:
                previous = os.environ.get("XDG_STATE_HOME")
                os.environ["XDG_STATE_HOME"] = state_home
                try:
                    first = default_journal_path(first_workspace)
                    self.assertEqual(first, default_journal_path(first_workspace))
                    self.assertNotEqual(first, default_journal_path(second_workspace))
                    self.assertTrue(first.startswith(state_home))
                    self.assertIn(workspace_identifier(first_workspace), first)
                    self.assertTrue(first.endswith(os.path.join("tasks.jsonl")))
                finally:
                    if previous is None:
                        os.environ.pop("XDG_STATE_HOME", None)
                    else:
                        os.environ["XDG_STATE_HOME"] = previous

    def test_default_journal_path_uses_persistent_user_state_fallback(self):
        previous = os.environ.pop("XDG_STATE_HOME", None)
        try:
            path = default_journal_path(os.path.dirname(__file__))
            expected_root = os.path.join(
                str(Path.home()), ".local", "state", "xv6-agent"
            )
            self.assertTrue(path.startswith(expected_root + os.sep))
        finally:
            if previous is not None:
                os.environ["XDG_STATE_HOME"] = previous

    def test_normal_application_enables_journal_by_default(self):
        with tempfile.TemporaryDirectory() as state_home, \
                tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("XDG_STATE_HOME")
            os.environ["XDG_STATE_HOME"] = state_home
            try:
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    run_application(["mock"], bridge=RecordingBridge(),
                                    workspace=workspace)
                path = default_journal_path(workspace)
                self.assertTrue(os.path.isfile(path))
                tasks = TaskJournal(
                    path, workspace_id=workspace_identifier(workspace)
                )._read_all()
                self.assertTrue(tasks)
                self.assertTrue(all(
                    task["workspace_id"] == workspace_identifier(workspace)
                    for task in tasks
                ))
            finally:
                if previous is None:
                    os.environ.pop("XDG_STATE_HOME", None)
                else:
                    os.environ["XDG_STATE_HOME"] = previous

    def test_pending_task_recovers_before_new_application_work(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = os.path.join(directory, "workspace")
            os.mkdir(workspace)
            path = os.path.join(directory, "journal.jsonl")
            workspace_id = workspace_identifier(workspace)
            journal = TaskJournal(path, workspace_id=workspace_id)
            task = journal.create_task(
                "append persisted operation",
                planner_id="agent.RulePlanner",
            )
            journal.update_task(task["task_id"], status="RUNNING")
            action = {
                "tool": "append_file",
                "args": {"path": "/report.txt", "data": "B"},
            }
            journal.mark_step(
                task["task_id"], 0, "PREPARED", action=action, effect_key=987654
            )

            bridge = RecordingBridge()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                run_application(["mock"], bridge=bridge, journal_path=path,
                                workspace=workspace)

            self.assertEqual(bridge.calls[0], ("append_file", action["args"], 987654))
            self.assertEqual(bridge.calls[1][0], "list_files")
            self.assertLess(
                output.getvalue().index("RECOVERED_TASK_COMPLETED="),
                output.getvalue().index("TEST"),
            )
            completed = journal.get_task(task["task_id"])
            self.assertEqual(completed["status"], "COMPLETED")
            self.assertEqual(completed["steps"][0]["action"], action)
            self.assertEqual(completed["steps"][0]["effect_key"], 987654)

    def test_completed_task_is_not_replayed_at_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = os.path.join(directory, "workspace")
            os.mkdir(workspace)
            path = os.path.join(directory, "journal.jsonl")
            workspace_id = workspace_identifier(workspace)
            journal = TaskJournal(path, workspace_id=workspace_id)
            task = journal.create_task(
                "already committed",
                planner_id="agent.RulePlanner",
            )
            action = {
                "tool": "append_file",
                "args": {"path": "/report.txt", "data": "Z"},
            }
            journal.mark_step(
                task["task_id"], 0, "COMPLETED", action=action,
                effect_key=13579, result={"ok": True, "output": "1"},
                task_status="COMPLETED",
            )

            bridge = RecordingBridge()
            with contextlib.redirect_stdout(io.StringIO()):
                run_application(["mock"], bridge=bridge, journal_path=path,
                                workspace=workspace)

            self.assertNotIn(
                ("append_file", action["args"], 13579), bridge.calls
            )
            self.assertEqual(bridge.files["/report.txt"].count("Z"), 0)


if __name__ == "__main__":
    unittest.main()
