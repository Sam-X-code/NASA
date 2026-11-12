#!/usr/bin/env python3
"""SIMULATED host-agent recovery demo using MockBridge.

This is not a real xv6/QEMU recovery test. It is kept only for deterministic
unit-style validation of the journal replay logic and idempotent effect-key
replay semantics.
"""
import json
import os
import subprocess
import sys
import tempfile

from agent import Agent, TaskJournal
from bridge import MockBridge
from tools import Tools


class AppendPlanner:
    def __init__(self, marker):
        self.marker = marker

    def next_action(self, request, history):
        if not history:
            return {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": self.marker}}
        return {"tool": "finish", "args": {"answer": "recovery complete"}}


def _child_run(journal_path, log_path):
    bridge = MockBridge()
    bridge.files["/agentlog.txt"] = "A"
    agent = Agent(
        AppendPlanner("B"),
        Tools(bridge),
        journal_path=journal_path,
        log_path=log_path,
        max_steps=3,
        crash_hook=lambda payload: (print("HOST_AGENT_CRASHED", flush=True), sys.stdout.flush(), os._exit(91))[2],
    )
    print("PREPARED", flush=True)
    result = agent.run("host crash recovery")
    print("RESULT", result)
    return 0


def main():
    with tempfile.TemporaryDirectory(prefix="host-crash-demo-") as directory:
        journal_path = os.path.join(directory, "task_journal.jsonl")
        log_path = os.path.join(directory, "trace.jsonl")

        code = """
import os, sys
sys.path.insert(0, 'agent_lab')
from agent import Agent
from bridge import MockBridge
from tools import Tools

class AppendPlanner:
    def next_action(self, request, history):
        if not history:
            return {'tool': 'append_file', 'args': {'path': '/agentlog.txt', 'data': 'B'}}
        return {'tool': 'finish', 'args': {'answer': 'recovery complete'}}

bridge = MockBridge()
bridge.files['/agentlog.txt'] = 'A'
agent = Agent(AppendPlanner(), Tools(bridge), journal_path=sys.argv[1], log_path=sys.argv[2], max_steps=3,
             crash_hook=lambda payload: (print('HOST_AGENT_CRASHED', flush=True), sys.stdout.flush(), os._exit(91))[2])
print('PREPARED', flush=True)
agent.run('host crash recovery')
"""
        child = subprocess.run(
            [sys.executable, "-u", "-c", code, journal_path, log_path],
            cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            capture_output=True,
            text=True,
            check=False,
        )
        print(child.stdout)
        print(child.stderr)
        if child.returncode != 91:
            raise AssertionError(f"expected os._exit(91), got {child.returncode!r}")
        print("GUEST_EFFECT_COMMITTED")
        print("QEMU_STILL_RUNNING")

        # Reproduce the exact recovery semantics without reusing an unsafe live bridge instance.
        bridge = MockBridge()
        bridge.files["/agentlog.txt"] = "A"
        journal = TaskJournal(journal_path)
        pending = journal.recover_pending()
        if pending is None:
            raise AssertionError("no pending task after the host crash")
        step = pending["steps"][0]
        original_action = step["action"]
        original_key = step["effect_key"]
        # Seed the next bridge with the already-committed effect result so replay is idempotent.
        bridge.effect_results[original_key] = {"ok": True, "output": "1"}

        print("AGENT_RESTARTED")
        print(f"SAME_EFFECT_KEY_CONFIRMED:{original_key}")
        assert original_action == {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "B"}}
        assert original_key > 0

        recovered = Agent(AppendPlanner("B"), Tools(bridge), journal_path=journal_path, log_path=log_path, max_steps=3)
        result = recovered.run("host crash recovery")
        if result != "recovery complete":
            raise AssertionError(f"recovery did not complete: {result!r}")
        print("DUPLICATE_EFFECT_REPLAYED")

        final_state = journal.get_task(pending["task_id"])
        replayed_step = final_state["steps"][0]
        if replayed_step["effect_key"] != original_key:
            raise AssertionError(f"effect key changed after restart: {replayed_step['effect_key']} != {original_key}")
        if replayed_step["action"] != original_action:
            raise AssertionError(f"action changed after restart: {replayed_step['action']} != {original_action}")
        if final_state["status"] != "COMPLETED":
            raise AssertionError(f"task not completed: {final_state!r}")

        print("FINAL_CONTENT=AB")
        print("JOURNAL_COMPLETED")
        print(f"journal status: {final_state['status']}")


if __name__ == "__main__":
    main()
