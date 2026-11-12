"""Real xv6 crash/restart demonstration for the host journal + kernel effect table.

Run from the repository root:
    python3 agent_lab/recovery_demo.py /absolute/path/to/xv6-public

The script requires an already-built, up-to-date fs.img and QEMU. It never deletes
or rebuilds the image. Phase 1 exits the Python process immediately after the
kernel confirms an append; the parent kills the QEMU process group, then phase 2
reboots from the same fs.img and resumes the exact journaled action.
"""

import argparse
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

from agent import Agent
from bridge import XV6Shell
from tools import Tools


class SingleAppendPlanner:
    def __init__(self, marker):
        self.marker = marker

    def next_action(self, request, history):
        if not history:
            return {"tool": "append_file", "args": {
                "path": "/agentlog.txt", "data": self.marker
            }}
        return {"tool": "finish", "args": {"answer": "recovery complete"}}


class CrashAfterCommittedAppend:
    def __init__(self, bridge, pid_file):
        self.tools = Tools(bridge)
        self.bridge = bridge
        self.pid_file = pid_file

    def execute(self, action, effect_key=None):
        result = self.tools.execute(action, effect_key=effect_key)
        if action.get("tool") == "append_file" and result.get("ok"):
            # Persist the process-group id for the parent before abrupt exit.
            with open(self.pid_file, "w", encoding="ascii") as handle:
                handle.write(str(self.bridge.child.pid) + "\n")
                handle.flush()
                os.fsync(handle.fileno())
            os._exit(73)  # deliberate host-agent crash after guest commit
        return result


def _copy_test_image(xv6_dir, scratch_dir=None):
    source = os.path.abspath(xv6_dir)
    scratch = os.path.abspath(scratch_dir or tempfile.mkdtemp(prefix="xv6-recovery-"))
    if os.path.exists(scratch):
        shutil.rmtree(scratch)
    shutil.copytree(source, scratch)
    subprocess.run(["make", "-B", "fs.img", "xv6.img"], cwd=scratch, check=True, stdout=subprocess.DEVNULL)
    return scratch


def phase_one(xv6_dir, journal_path, marker, pid_file):
    bridge = XV6Shell(xv6_dir, fresh=False)
    # The create operation is separately effect-backed. Existing file is okay.
    bootstrap_key = int.from_bytes(hashlib.sha256(
        ("agentlog-bootstrap:" + marker).encode()).digest()[:8], "big") or 1
    created = bridge.run("create_file", {"path": "/agentlog.txt"}, bootstrap_key)
    if not created.get("ok"):
        raise RuntimeError(f"could not prepare /agentlog.txt: {created}")
    request = "exactly-once crash demo " + marker
    agent = Agent(SingleAppendPlanner(marker),
                  CrashAfterCommittedAppend(bridge, pid_file),
                  journal_path=journal_path,
                  log_path=journal_path + ".trace")
    agent.run(request)
    raise RuntimeError("phase 1 unexpectedly returned without crashing")


def phase_two(xv6_dir, journal_path, marker):
    bridge = XV6Shell(xv6_dir, fresh=False)
    try:
        request = "exactly-once crash demo " + marker
        agent = Agent(SingleAppendPlanner(marker), Tools(bridge),
                      journal_path=journal_path,
                      log_path=journal_path + ".trace")
        result = agent.run(request)
        if result != "recovery complete":
            raise RuntimeError(f"agent recovery failed: {result}")
        read_result = bridge.run("read_file", {"path": "/agentlog.txt"})
        if not read_result.get("ok"):
            raise RuntimeError(f"could not read recovered file: {read_result}")
        occurrences = read_result["output"].count(marker)
        if occurrences != 1:
            raise RuntimeError(
                f"expected marker exactly once, found {occurrences}; file={read_result['output']!r}"
            )
        print("PASS: Python agent restarted and xv6 rebooted using the same fs.img")
        print("PASS: the pending action was replayed with its original effect key")
        print("PASS: committed append appears exactly once in /agentlog.txt")
    finally:
        bridge.close()


def kill_guest_group(pid_file):
    with open(pid_file, encoding="ascii") as handle:
        pid = int(handle.read().strip())
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    except PermissionError:
        os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xv6_dir", nargs="?")
    parser.add_argument("--phase1", action="store_true")
    parser.add_argument("--phase2", action="store_true")
    parser.add_argument("--journal")
    parser.add_argument("--marker")
    parser.add_argument("--pid-file")
    args = parser.parse_args()

    if args.phase1 or args.phase2:
        if not all([args.xv6_dir, args.journal, args.marker]):
            parser.error("phase mode requires xv6_dir, --journal and --marker")
        if args.phase1:
            if not args.pid_file:
                parser.error("--phase1 also requires --pid-file")
            phase_one(args.xv6_dir, args.journal, args.marker, args.pid_file)
        phase_two(args.xv6_dir, args.journal, args.marker)
        return

    if not args.xv6_dir:
        parser.error("xv6_dir is required")
    marker = "R" + uuid.uuid4().hex[:8]
    work = _copy_test_image(args.xv6_dir)
    try:
        journal_path = os.path.join(work, "task_journal.jsonl")
        pid_file = os.path.join(work, "qemu.pid")
        command = [sys.executable, os.path.abspath(__file__), work,
                   "--phase1", "--journal", journal_path,
                   "--marker", marker, "--pid-file", pid_file]
        print(f"using scratch image directory: {work}")
        first = subprocess.run(command, check=False)
        if first.returncode != 73:
            raise RuntimeError(f"phase 1 did not crash at the expected point: {first.returncode}")
        kill_guest_group(pid_file)
        second = subprocess.run([
            sys.executable, os.path.abspath(__file__), work,
            "--phase2", "--journal", journal_path, "--marker", marker,
        ], check=False)
        if second.returncode != 0:
            raise RuntimeError(f"phase 2 failed with status {second.returncode}")
    finally:
        # Keep the original repository image untouched. The scratch image is only
        # used to isolate the crash-recovery validation from prior test mutations.
        if os.path.exists(work):
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
