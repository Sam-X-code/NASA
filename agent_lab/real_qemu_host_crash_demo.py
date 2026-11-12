#!/usr/bin/env python3
"""Real-QEMU host-agent crash recovery demo.

This script owns a single XV6Shell instance in the parent process and exposes a
small IPC bridge to a child Python agent. The child agent executes against the
real guest effect() path without creating a second QEMU instance. The same live
bridge and the same journal are reused across the crash and recovery steps.
"""

import multiprocessing as mp
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from agent import Agent, TaskJournal, _stable_effect_key
from bridge import XV6Shell
from tools import Tools


class AppendPlanner:
    def __init__(self, marker):
        self.marker = marker

    def next_action(self, request, history):
        if not history:
            return {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": self.marker}}
        return {"tool": "finish", "args": {"answer": "recovery complete"}}


class BridgeProxy:
    def __init__(self, conn):
        self.conn = conn

    def run(self, tool, args, effect_key=None):
        self.conn.send({"kind": "run", "tool": tool, "args": args, "effect_key": effect_key})
        reply = self.conn.recv()
        if isinstance(reply, dict) and "error" in reply:
            raise RuntimeError(reply["error"])
        return reply

    def shell(self, command):
        self.conn.send({"kind": "shell", "command": command})
        reply = self.conn.recv()
        if isinstance(reply, dict) and "error" in reply:
            raise RuntimeError(reply["error"])
        return reply


def bridge_service_loop(conn, bridge):
    while True:
        try:
            message = conn.recv()
        except EOFError:
            break
        if message is None:
            break
        kind = message.get("kind")
        try:
            if kind == "shell":
                result = bridge._shell(message["command"])
            elif kind == "run":
                result = bridge.run(message["tool"], message["args"], message.get("effect_key"))
            else:
                result = {"error": f"unknown bridge request: {kind!r}"}
        except Exception as exc:  # pragma: no cover - exercised only under real QEMU
            result = {"error": str(exc)}
        conn.send(result)


def _run_agent_child(request, journal_path, trace_path, conn, crash_hook=None):
    proxy = BridgeProxy(conn)
    if crash_hook is None:
        crash_hook = lambda _payload: False
    agent = Agent(
        AppendPlanner("B"),
        Tools(proxy),
        journal_path=journal_path,
        log_path=trace_path,
        max_steps=5,
        crash_hook=crash_hook,
    )
    agent.run(request)


def _create_workdir_and_build():
    root = tempfile.mkdtemp(prefix="xv6-real-qemu-crash-")
    repo_copy = os.path.join(root, "repo")
    shutil.copytree(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), repo_copy)
    subprocess.run(["make", "-B", "fs.img", "xv6.img"], cwd=repo_copy,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, check=True)
    return root, repo_copy


def _start_agent(request, journal_path, trace_path, conn, crash_hook=None):
    proc = mp.get_context("fork").Process(
        target=_run_agent_child,
        args=(request, journal_path, trace_path, conn, crash_hook),
    )
    proc.start()
    return proc


def main():
    root, repo_copy = _create_workdir_and_build()
    journal_path = os.path.join(root, "task_journal.jsonl")
    trace_path = os.path.join(root, "trace.jsonl")
    bridge = None
    service_thread = None
    parent_conn = None
    try:
        bridge = XV6Shell(repo_copy, fresh=False, timeout=30)
        parent_conn, child_conn = mp.Pipe(duplex=True)
        service_thread = threading.Thread(target=bridge_service_loop, args=(parent_conn, bridge), daemon=True)
        service_thread.start()

        # Initial guest state: create and verify A before the agent starts using
        # the same effect-backed path that the real agent uses.
        create_key = _stable_effect_key("seed-create", 0, {"tool": "create_file", "args": {"path": "/agentlog.txt"}})
        seed_append_key = _stable_effect_key("seed-append", 0, {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "A"}})
        create_result = bridge.run("create_file", {"path": "/agentlog.txt"}, create_key)
        if create_result.get("ok") is not True:
            raise AssertionError(f"create_file seed step failed: {create_result!r}")
        append_result = bridge.run("append_file", {"path": "/agentlog.txt", "data": "A"}, seed_append_key)
        if append_result.get("ok") is not True:
            raise AssertionError(f"seed append step failed: {append_result!r}")
        initial = bridge.run("read_file", {"path": "/agentlog.txt"})
        if initial.get("ok") is not True or initial.get("output") != "A":
            raise AssertionError(f"initial file content is not A: {initial!r}")
        print("INITIAL_FILE_OK")

        request = "append B to /agentlog.txt exactly once"
        print("PREPARED")
        crash_hook = lambda _payload: (print("HOST_AGENT_CRASHED", flush=True), os._exit(91))[1]
        agent_proc = _start_agent(request, journal_path, trace_path, child_conn, crash_hook=crash_hook)
        agent_proc.join()
        if agent_proc.exitcode != 91:
            raise AssertionError(f"expected child exit code 91; got {agent_proc.exitcode!r}")
        print("CHILD_EXIT=91")

        if not bridge.child.isalive():
            raise AssertionError("QEMU died when the host Python agent exited")
        after_crash = bridge.run("read_file", {"path": "/agentlog.txt"})
        if after_crash.get("ok") is not True or after_crash.get("output") != "AB":
            raise AssertionError(f"guest file after host crash is not AB: {after_crash!r}")
        print("GUEST_EFFECT_COMMITTED")
        print("QEMU_STILL_RUNNING")

        journal = TaskJournal(journal_path)
        task = journal.recover_pending()
        if task is None:
            raise AssertionError("journal had no pending task after the crash")
        pending_step = task["steps"][0]
        original_action = pending_step.get("action")
        original_key = pending_step.get("effect_key")
        if original_action is None or original_key is None:
            raise AssertionError(f"pending journal entry missing action or effect key: {pending_step!r}")
        if original_action != {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "B"}}:
            raise AssertionError(f"pending action changed after crash: {original_action!r}")
        if not isinstance(original_key, int) or original_key <= 0 or original_key >= (1 << 64):
            raise AssertionError(f"effect key is invalid: {original_key!r}")
        print("JOURNAL_RECOVERABLE")

        # Restart the agent against the same journal and same live bridge. It must
        # replay the original pending action and original effect key.
        parent_conn.close()
        parent_conn, child_conn = mp.Pipe(duplex=True)
        service_thread.join(timeout=0.1)
        service_thread = threading.Thread(target=bridge_service_loop, args=(parent_conn, bridge), daemon=True)
        service_thread.start()

        print("AGENT_RESTARTED")
        restarted = _start_agent(request, journal_path, trace_path, child_conn)
        restarted.join()
        if restarted.exitcode != 0:
            raise AssertionError(f"restarted agent should succeed; exit code was {restarted.exitcode!r}")

        final = bridge.run("read_file", {"path": "/agentlog.txt"})
        if final.get("ok") is not True or final.get("output") != "AB":
            raise AssertionError(f"final guest file after replay is not AB: {final!r}")
        print("FINAL_CONTENT=AB")

        final_task = TaskJournal(journal_path).get_task(task["task_id"])
        if final_task is None:
            raise AssertionError("journal task missing after recovery")
        if final_task["status"] != "COMPLETED":
            raise AssertionError(f"journal is not completed after replay: {final_task!r}")
        replayed_step = final_task["steps"][0]
        if replayed_step.get("effect_key") != original_key:
            raise AssertionError(f"restarted agent did not reuse original key: {replayed_step.get('effect_key')!r} != {original_key!r}")
        if replayed_step.get("action") != original_action:
            raise AssertionError(f"restarted agent did not reuse original action: {replayed_step.get('action')!r} != {original_action!r}")
        print("JOURNAL_COMPLETED")
        print(f"SAME_EFFECT_KEY_CONFIRMED:{original_key}")
        print("DUPLICATE_EFFECT_REPLAYED")
    finally:
        try:
            if parent_conn is not None:
                parent_conn.send(None)
            if service_thread is not None:
                service_thread.join(timeout=1)
        except Exception:
            pass
        if bridge is not None:
            bridge.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
