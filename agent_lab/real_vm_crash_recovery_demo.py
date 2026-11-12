#!/usr/bin/env python3
"""Exercise a real xv6 effect across a forced QEMU crash and agent restart."""

import hashlib
import multiprocessing as mp
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

from agent import Agent, TaskJournal, _stable_effect_key
from bridge import XV6Shell
from tools import Tools


REQUEST = "Append B exactly once to /agentlog.txt"
ACTION = {"tool": "append_file", "args": {"path": "/agentlog.txt", "data": "B"}}
EXPECTED_FILE = "/agentlog.txt"


class AppendPlanner:
    def __init__(self, recovery=False):
        self.recovery = recovery

    def next_action(self, request, history):
        if not history:
            if self.recovery:
                raise AssertionError("recovery planner was asked to regenerate a pending action")
            return ACTION
        if history[-1]["action"] != ACTION:
            raise AssertionError(f"unexpected recovery history: {history!r}")
        return {"tool": "finish", "args": {"answer": "recovery complete"}}


class BridgeProxy:
    def __init__(self, connection):
        self.connection = connection

    def run(self, tool, args, effect_key=None):
        self.connection.send({
            "kind": "run",
            "tool": tool,
            "args": args,
            "effect_key": effect_key,
        })
        return self._receive()

    def crash_vm(self):
        self.connection.send({"kind": "crash_vm"})
        return self._receive()

    def _receive(self):
        response = self.connection.recv()
        if isinstance(response, dict) and "error" in response:
            raise RuntimeError(response["error"])
        return response


def _wait_for_qemu_exit(bridge, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not bridge.child.isalive():
            return
        time.sleep(0.02)
    raise AssertionError(f"QEMU PID {bridge.child.pid} did not exit after SIGKILL")


def _image_snapshot(path):
    stat = os.stat(path)
    return {
        "path": os.path.abspath(path),
        "inode": stat.st_ino,
        "size": stat.st_size,
        "sha256": _sha256(path),
    }


def _bridge_service(connection, bridge, events):
    while True:
        try:
            message = connection.recv()
        except EOFError:
            return
        if message is None:
            return
        try:
            if message["kind"] == "run":
                result = bridge.run(
                    message["tool"], message["args"], message.get("effect_key")
                )
                events.append({
                    "kind": "run",
                    "tool": message["tool"],
                    "args": message["args"],
                    "effect_key": message.get("effect_key"),
                    "result": result,
                })
            elif message["kind"] == "crash_vm":
                guest = bridge.run("read_file", {"path": EXPECTED_FILE})
                if guest.get("ok") is not True or guest.get("output") != "AB":
                    raise AssertionError(
                        f"pre-crash guest content was not AB: {guest!r}"
                    )
                image_after_effect = _image_snapshot(bridge.fs_img_path)
                pid = bridge.child.pid
                os.kill(pid, signal.SIGKILL)
                _wait_for_qemu_exit(bridge)
                if bridge.child.signalstatus != signal.SIGKILL:
                    raise AssertionError(
                        "QEMU exited without SIGKILL: "
                        f"exitstatus={bridge.child.exitstatus}, "
                        f"signalstatus={bridge.child.signalstatus}"
                    )
                result = {
                    "content": guest["output"],
                    "pid": pid,
                    "signal": bridge.child.signalstatus,
                    "image_after_effect": image_after_effect,
                }
                events.append({"kind": "crash_vm", **result})
            else:
                raise ValueError(f"unknown bridge request: {message['kind']!r}")
            connection.send(result)
            if message["kind"] == "crash_vm":
                return
        except Exception as exc:
            events.append({"kind": "error", "error": repr(exc)})
            connection.send({"error": str(exc)})


def _run_agent(request, journal_path, trace_path, connection, crash_vm=False,
               recovery=False):
    proxy = BridgeProxy(connection)

    def crash_hook(_payload):
        if crash_vm:
            result = proxy.crash_vm()
            if result.get("content") != "AB" or result.get("signal") != signal.SIGKILL:
                raise AssertionError(f"QEMU crash was not verified: {result!r}")
            print("Agent crash hook verified QEMU SIGKILL", flush=True)
            return True
        return False

    agent = Agent(
        AppendPlanner(recovery=recovery),
        Tools(proxy),
        journal_path=journal_path,
        log_path=trace_path,
        max_steps=5,
        crash_hook=crash_hook,
        crash_exit_code=91,
    )
    agent.run(request)


def _copy_repo_and_build():
    root = tempfile.mkdtemp(prefix="xv6-real-vm-crash-")
    repo_copy = os.path.join(root, "repo")
    source = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    shutil.copytree(
        source,
        repo_copy,
        ignore=shutil.ignore_patterns(".git", "__pycache__"),
    )
    build = subprocess.run(
        ["make", "-B", "fs.img", "xv6.img"],
        cwd=repo_copy,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if build.returncode:
        raise RuntimeError(
            f"disposable xv6 image build failed ({build.returncode}):\n{build.stdout}"
        )
    return root, repo_copy, build.stdout


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as image:
        for chunk in iter(lambda: image.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _qemu_paths(bridge):
    args = [arg.decode() if isinstance(arg, bytes) else arg
            for arg in bridge.child.args]
    drives = [args[i + 1] for i, arg in enumerate(args[:-1]) if arg == "-drive"]
    fs_arg = next((drive for drive in drives if "index=1" in drive), None)
    xv6_arg = next((drive for drive in drives if "index=0" in drive), None)
    return args, fs_arg, xv6_arg


def _raw_metadata(fs_img, effect_key):
    with open(fs_img, "rb") as image:
        raw = image.read()
    if len(raw) < 1024:
        raise AssertionError("filesystem image is too small to contain a superblock")
    superblock = struct.unpack_from("<9I", raw, 512)
    size, _, _, _, logstart, inodestart, bmapstart, effstart, effblocks = superblock
    if size * 512 > len(raw):
        raise AssertionError("superblock filesystem size exceeds the image")
    ipb = 512 // 64

    def read_inode(inum):
        offset = (inodestart + inum // ipb) * 512 + (inum % ipb) * 64
        return struct.unpack_from("<hhhhI13I", raw, offset)

    root = read_inode(1)
    root_block_count = (root[4] + 511) // 512
    root_blocks = []
    root_entries = []
    for logical_block in range(root_block_count):
        if logical_block >= 12:
            raise AssertionError("raw inspector does not support indirect root entries")
        block = root[5 + logical_block]
        if block == 0:
            raise AssertionError(f"root inode lacks block pointer {logical_block}")
        root_blocks.append(block)
        bytes_in_block = min(512, root[4] - logical_block * 512)
        for offset in range(0, bytes_in_block, 16):
            position = block * 512 + offset
            inum = struct.unpack_from("<H", raw, position)[0]
            name = raw[position + 2:position + 16].split(b"\0", 1)[0].decode(
                "ascii", "replace"
            )
            if inum:
                root_entries.append((name, inum, block))

    file_entry = next(
        (entry for entry in root_entries if entry[0] == "agentlog.txt"), None
    )
    if file_entry is None:
        raise AssertionError("raw filesystem directory lacks agentlog.txt")
    file_inode = read_inode(file_entry[1])
    remaining = file_inode[4]
    contents = bytearray()
    for block in file_inode[5:17]:
        if remaining <= 0:
            break
        if block == 0:
            raise AssertionError("agentlog inode has a missing data block")
        count = min(512, remaining)
        contents.extend(raw[block * 512:block * 512 + count])
        remaining -= count
    if remaining:
        raise AssertionError("agentlog content exceeds supported direct blocks")

    bitmap = []
    for block in root_blocks:
        bitmap_byte = raw[bmapstart * 512 + block // 8]
        bitmap.append((block, bool(bitmap_byte & (1 << (block % 8)))))

    capacity = effblocks * (512 // 16)
    index = (effect_key & 0xFFFFFFFF) % capacity
    ept_result = None
    for _ in range(capacity):
        block = effstart + index // (512 // 16)
        slot = index % (512 // 16)
        key, result = struct.unpack_from("<QQ", raw, block * 512 + slot * 16)
        if key == effect_key:
            ept_result = result
            break
        if key == 0:
            break
        index = (index + 1) % capacity
    log_count = struct.unpack_from("<I", raw, logstart * 512)[0]
    return {
        "superblock": superblock,
        "root_size": root[4],
        "root_blocks": root_blocks,
        "root_entries": root_entries,
        "file_inode": file_entry[1],
        "file_entry_block": file_entry[2],
        "file_bytes": bytes(contents),
        "bitmap": bitmap,
        "ept_result": ept_result,
        "log_count": log_count,
        "image_size": len(raw),
        "image_inode": os.stat(fs_img).st_ino,
        "image_sha256": _sha256(fs_img),
    }


def _start_service_agent(request, journal_path, trace_path, bridge, *,
                         crash_vm=False, recovery=False):
    context = mp.get_context("fork")
    parent_connection, child_connection = context.Pipe(duplex=True)
    process = context.Process(
        target=_run_agent,
        args=(request, journal_path, trace_path, child_connection, crash_vm, recovery),
    )
    process.start()
    child_connection.close()
    events = []
    service = threading.Thread(
        target=_bridge_service,
        args=(parent_connection, bridge, events),
        daemon=True,
    )
    service.start()
    return process, parent_connection, service, events


def _stop_service(connection, service):
    try:
        connection.send(None)
    except (BrokenPipeError, EOFError, OSError):
        pass
    service.join(timeout=3)
    connection.close()
    if service.is_alive():
        raise AssertionError("bridge service thread did not stop")


def main():
    root = None
    bridge1 = None
    bridge2 = None
    agent_process = None
    service_connection = None
    service_thread = None
    try:
        root, repo_copy, build_output = _copy_repo_and_build()
        print("DISPOSABLE_BUILD=PASS")
        fs_img = os.path.abspath(os.path.join(repo_copy, "fs.img"))
        xv6_img = os.path.abspath(os.path.join(repo_copy, "xv6.img"))
        journal_path = os.path.join(root, "task_journal.jsonl")
        trace_path = os.path.join(root, "trace.jsonl")
        initial_image = _image_snapshot(fs_img)
        initial_image_hash = initial_image["sha256"]
        initial_image_inode = initial_image["inode"]
        initial_image_size = initial_image["size"]
        print(f"FS_IMG={fs_img}")
        print(f"INITIAL_IMAGE inode={initial_image_inode} size={initial_image_size} "
              f"sha256={initial_image_hash}")

        bridge1 = XV6Shell(repo_copy, fresh=False, timeout=30, disk_cache="directsync")
        args1, fs_drive1, xv6_drive1 = _qemu_paths(bridge1)
        if fs_drive1 is None or fs_img not in fs_drive1:
            raise AssertionError(f"first QEMU is not using expected fs.img: {args1!r}")
        if xv6_drive1 is None or xv6_img not in xv6_drive1:
            raise AssertionError(f"first QEMU boot disk is unexpected: {args1!r}")
        print(f"QEMU1_PID={bridge1.child.pid}")
        print(f"QEMU1_ARGS={args1!r}")
        print(f"QEMU1_FS_DRIVE={fs_drive1}")

        create_key = _stable_effect_key(
            "vm-crash-seed", 0,
            {"tool": "create_file", "args": {"path": EXPECTED_FILE}},
        )
        seed_key = _stable_effect_key(
            "vm-crash-seed", 1,
            {"tool": "append_file", "args": {"path": EXPECTED_FILE, "data": "A"}},
        )
        for tool, args, key in (
            ("create_file", {"path": EXPECTED_FILE}, create_key),
            ("append_file", {"path": EXPECTED_FILE, "data": "A"}, seed_key),
        ):
            result = bridge1.run(tool, args, effect_key=key)
            if result.get("ok") is not True:
                raise AssertionError(f"guest seed {tool} failed: {result!r}")
        initial = bridge1.run("read_file", {"path": EXPECTED_FILE})
        if initial.get("ok") is not True or initial.get("output") != "A":
            raise AssertionError(f"initial guest file was not exactly A: {initial!r}")
        print("INITIAL_FILE_OK")

        journal = TaskJournal(journal_path)
        task = journal.create_task(REQUEST)
        task_id = task["task_id"]
        journal.update_task(task_id, status="RUNNING")
        step_id = 0
        effect_key = _stable_effect_key(task_id, step_id, ACTION)
        journal.mark_step(
            task_id, step_id, "PREPARED", action=ACTION, effect_key=effect_key
        )
        prepared = journal.get_task(task_id)
        prepared_step = prepared["steps"][step_id]
        if (prepared["status"] != "RUNNING"
                or prepared_step["status"] != "PREPARED"
                or prepared_step["action"] != ACTION
                or prepared_step["effect_key"] != effect_key):
            raise AssertionError(f"PREPARED journal record did not persist: {prepared!r}")
        print(f"HOST_JOURNAL_PREPARED task_id={task_id} step_id={step_id} "
              f"effect_key={effect_key}")

        # Execute through Agent so its post-success/pre-completion crash hook
        # performs the actual VM kill before the durable completion checkpoint.
        agent_process, service_connection, service_thread, first_events = (
            _start_service_agent(
                REQUEST, journal_path, trace_path, bridge1, crash_vm=True
            )
        )
        agent_process.join(timeout=30)
        if agent_process.is_alive():
            raise AssertionError("host agent did not reach its crash hook in time")
        if agent_process.exitcode != 91:
            raise AssertionError(
                f"host agent should exit via os._exit(91), got {agent_process.exitcode}"
            )
        _stop_service(service_connection, service_thread)
        service_connection = service_thread = None

        first_append_events = [
            event for event in first_events
            if event.get("kind") == "run" and event.get("tool") == "append_file"
        ]
        if len(first_append_events) != 1:
            raise AssertionError(f"expected exactly one agent append call: {first_events!r}")
        first_append = first_append_events[0]
        if first_append["args"] != ACTION["args"] or first_append["effect_key"] != effect_key:
            raise AssertionError(f"agent executed different action/key: {first_append!r}")
        if first_append["result"].get("ok") is not True:
            raise AssertionError(f"guest effect failed: {first_append!r}")
        crash_event = next(
            (event for event in first_events if event.get("kind") == "crash_vm"), None
        )
        if crash_event is None or crash_event["content"] != "AB":
            raise AssertionError(f"pre-crash commit was not verified: {first_events!r}")
        image_after_effect = crash_event["image_after_effect"]
        if (image_after_effect["path"] != fs_img
                or image_after_effect["inode"] != initial_image_inode
                or image_after_effect["size"] != initial_image_size):
            raise AssertionError(
                f"filesystem image identity changed before crash: {image_after_effect!r}"
            )
        print(
            "IMAGE_AFTER_EFFECT "
            f"path={image_after_effect['path']} inode={image_after_effect['inode']} "
            f"size={image_after_effect['size']} sha256={image_after_effect['sha256']}"
        )
        print("GUEST_EFFECT_COMMITTED")
        print("CONTENT_BEFORE_CRASH=AB")
        print("QEMU_KILLED_WITH_SIGKILL")
        if bridge1.child.isalive() or bridge1.child.signalstatus != signal.SIGKILL:
            raise AssertionError(
                f"QEMU did not exit due to SIGKILL: "
                f"alive={bridge1.child.isalive()}, "
                f"signalstatus={bridge1.child.signalstatus}, "
                f"exitstatus={bridge1.child.exitstatus}"
            )
        print("FIRST_QEMU_EXITED")

        pending = TaskJournal(journal_path).recover_pending()
        if pending is None:
            raise AssertionError("no unfinished journal task after agent crash")
        pending_step = pending["steps"][step_id]
        if pending["task_id"] != task_id:
            raise AssertionError("recovered task ID changed")
        if pending_step.get("step_index") != step_id:
            raise AssertionError("recovered step index changed")
        if pending_step.get("status") != "RUNNING":
            raise AssertionError(f"expected RUNNING step after crash: {pending_step!r}")
        if pending_step.get("action") != ACTION:
            raise AssertionError(f"original action changed in journal: {pending_step!r}")
        if pending_step.get("effect_key") != effect_key:
            raise AssertionError(f"original effect key changed: {pending_step!r}")
        image_after_crash = _image_snapshot(fs_img)
        if (image_after_crash["inode"] != initial_image_inode
                or image_after_crash["size"] != initial_image_size):
            raise AssertionError("filesystem image disappeared or changed size")
        print(
            "IMAGE_AFTER_CRASH "
            f"path={image_after_crash['path']} inode={image_after_crash['inode']} "
            f"size={image_after_crash['size']} sha256={image_after_crash['sha256']}"
        )

        if bridge1.fs_img_path != fs_img:
            raise AssertionError("first bridge path no longer matches fs.img")
        bridge1.child.close(force=False)
        try:
            os.unlink(bridge1.monitor_path)
        except FileNotFoundError:
            pass

        bridge2 = XV6Shell(repo_copy, fresh=False, timeout=30, disk_cache="directsync")
        args2, fs_drive2, xv6_drive2 = _qemu_paths(bridge2)
        if fs_drive2 is None or fs_img not in fs_drive2:
            raise AssertionError(f"second QEMU is not using same fs.img: {args2!r}")
        if xv6_drive2 is None or xv6_img not in xv6_drive2:
            raise AssertionError(f"second QEMU boot disk is unexpected: {args2!r}")
        if bridge2.fs_img_path != bridge1.fs_img_path:
            raise AssertionError("second boot resolved a different filesystem image path")
        if not bridge2.child.isalive():
            raise AssertionError("second QEMU exited before the recovery checks")
        print("SAME_FS_IMAGE_REUSED")
        print(f"QEMU2_PID={bridge2.child.pid}")
        print(f"QEMU2_ARGS={args2!r}")
        print(f"QEMU2_FS_DRIVE={fs_drive2}")
        print(f"SECOND_QEMU_STARTED pid={bridge2.child.pid} fs_img={bridge2.fs_img_path}")

        after_reboot = bridge2.run("read_file", {"path": EXPECTED_FILE})
        if after_reboot.get("ok") is not True or after_reboot.get("output") != "AB":
            raise AssertionError(
                f"guest file not exactly AB after reboot, before replay: {after_reboot!r}"
            )
        print("CONTENT_AFTER_REBOOT_BEFORE_REPLAY=AB")
        image_after_reboot = _image_snapshot(fs_img)
        if (image_after_reboot["inode"] != initial_image_inode
                or image_after_reboot["size"] != initial_image_size):
            raise AssertionError(
                f"filesystem image identity changed on reboot: {image_after_reboot!r}"
            )
        print(
            "IMAGE_AFTER_REBOOT_BEFORE_REPLAY "
            f"path={image_after_reboot['path']} inode={image_after_reboot['inode']} "
            f"size={image_after_reboot['size']} sha256={image_after_reboot['sha256']}"
        )
        metadata_before_replay = _raw_metadata(fs_img, effect_key)
        if metadata_before_replay["file_bytes"] != b"AB":
            raise AssertionError("raw file contents before replay are not AB")
        if metadata_before_replay["ept_result"] != 1:
            raise AssertionError("EPT key/result missing before replay")
        if metadata_before_replay["log_count"] != 0:
            raise AssertionError("WAL log header is not clean before replay")
        print(
            "RAW_EPT_BEFORE_REPLAY "
            f"key={effect_key} saved_result={metadata_before_replay['ept_result']} "
            f"WAL_header_n={metadata_before_replay['log_count']}"
        )

        recovered_step = pending["steps"][step_id]
        if (recovered_step["action"] != ACTION
                or recovered_step["action"]["args"]["path"] != EXPECTED_FILE
                or recovered_step["action"]["args"]["data"] != "B"
                or recovered_step["effect_key"] != effect_key
                or pending["task_id"] != task_id
                or recovered_step["step_index"] != step_id):
            raise AssertionError(f"recovered operation identity mismatch: {pending!r}")
        print(f"ORIGINAL_EFFECT_KEY_RECOVERED={recovered_step['effect_key']}")
        print(f"RECOVERED_TASK_ID={pending['task_id']} RECOVERED_STEP_ID={step_id}")

        recovery_process, service_connection, service_thread, recovery_events = (
            _start_service_agent(
                REQUEST, journal_path, trace_path, bridge2, recovery=True
            )
        )
        print("AGENT_RESTARTED")
        recovery_process.join(timeout=30)
        if recovery_process.is_alive():
            raise AssertionError("restarted agent did not finish in time")
        if recovery_process.exitcode != 0:
            raise AssertionError(
                f"restarted agent failed with exit code {recovery_process.exitcode}; "
                f"bridge events={recovery_events!r}"
            )
        _stop_service(service_connection, service_thread)
        service_connection = service_thread = None

        replay_events = [
            event for event in recovery_events
            if event.get("kind") == "run" and event.get("tool") == "append_file"
        ]
        if len(replay_events) != 1:
            raise AssertionError(f"expected one recovery append replay: {recovery_events!r}")
        replay = replay_events[0]
        if replay["args"] != ACTION["args"] or replay["effect_key"] != effect_key:
            raise AssertionError(f"replay did not use original action/key: {replay!r}")
        if replay["result"].get("ok") is not True or replay["result"].get("output") != "1":
            raise AssertionError(f"duplicate effect did not return original result: {replay!r}")
        print("DUPLICATE_EFFECT_REPLAYED")

        final_file = bridge2.run("read_file", {"path": EXPECTED_FILE})
        if final_file.get("ok") is not True or final_file.get("output") != "AB":
            raise AssertionError(f"file changed on replay: {final_file!r}")
        print("FINAL_CONTENT=AB")
        image_after_replay = _image_snapshot(fs_img)
        if (image_after_replay["inode"] != initial_image_inode
                or image_after_replay["size"] != initial_image_size):
            raise AssertionError(
                f"filesystem image identity changed after replay: {image_after_replay!r}"
            )
        print(
            "IMAGE_AFTER_REPLAY "
            f"path={image_after_replay['path']} inode={image_after_replay['inode']} "
            f"size={image_after_replay['size']} sha256={image_after_replay['sha256']}"
        )

        completed = TaskJournal(journal_path).get_task(task_id)
        if completed is None or completed.get("status") != "COMPLETED":
            raise AssertionError(f"journal task not COMPLETED: {completed!r}")
        completed_step = completed["steps"][step_id]
        if (completed_step.get("status") != "COMPLETED"
                or completed_step.get("action") != ACTION
                or completed_step.get("effect_key") != effect_key):
            raise AssertionError(f"completed journal step changed: {completed_step!r}")
        print("JOURNAL_COMPLETED")

        bridge2.close()
        bridge2 = None
        metadata = _raw_metadata(fs_img, effect_key)
        if metadata["file_bytes"] != b"AB":
            raise AssertionError(f"raw filesystem file bytes are not AB: {metadata!r}")
        if metadata["ept_result"] != 1:
            raise AssertionError(f"raw EPT lacks committed key/result: {metadata!r}")
        if metadata["log_count"] != 0:
            raise AssertionError(f"WAL log header is not clean: {metadata!r}")
        if metadata["file_entry_block"] not in metadata["root_blocks"]:
            raise AssertionError(f"root inode does not reference file entry: {metadata!r}")
        if any(not allocated for _, allocated in metadata["bitmap"]):
            raise AssertionError(f"root directory block is not allocated: {metadata!r}")
        print(
            "RAW_METADATA_OK "
            f"root_size={metadata['root_size']} root_blocks={metadata['root_blocks']} "
            f"file_inode={metadata['file_inode']} "
            f"directory_block={metadata['file_entry_block']} "
            f"EPT_result={metadata['ept_result']} WAL_header_n={metadata['log_count']}"
        )
        print("REAL_VM_CRASH_RECOVERY_PASS")
    except Exception as exc:
        print(f"REAL_VM_CRASH_RECOVERY_FAILED: {exc}", file=sys.stderr, flush=True)
        raise
    finally:
        if service_connection is not None and service_thread is not None:
            try:
                _stop_service(service_connection, service_thread)
            except Exception as cleanup_error:
                print(f"bridge service cleanup warning: {cleanup_error}", file=sys.stderr)
        if agent_process is not None and agent_process.is_alive():
            agent_process.terminate()
            agent_process.join(timeout=5)
        for bridge in (bridge2, bridge1):
            if bridge is None:
                continue
            try:
                if bridge.child.isalive():
                    bridge.close()
                else:
                    bridge.child.close(force=False)
                    try:
                        os.unlink(bridge.monitor_path)
                    except FileNotFoundError:
                        pass
            except Exception as cleanup_error:
                print(f"QEMU cleanup warning: {cleanup_error}", file=sys.stderr)
        if root is not None:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
