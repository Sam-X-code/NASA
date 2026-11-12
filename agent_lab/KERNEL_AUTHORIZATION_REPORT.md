# Kernel Authorization and Persistence Audit

## Current result

The kernel enforces the agreed shared-agent capability, an exact four-path
allowlist, and CREATE/APPEND payload rules before looking up or inserting an
EPT key. Direct syscalls from ordinary and untrusted guest programs were
denied in the guest tests. Authorized effects retain the existing EPT key/result
format and WAL transaction path.

The capability is deliberately shared by every process that successfully
executes the trusted `/effectctl` inode. It is not host-agent authentication
and does not isolate trusted `effectctl` callers from one another.

## Authorization and trust enforcement

- [effect.c](../effect.c): `effect_init()` records the trusted `/effectctl`
  device/inode. `effect_authorized_exec()` compares a successfully loaded
  executable with that identity. The exact kernel path allowlist is
  `/report.txt`, `/inbox.txt`, `/important.txt`, and `/agentlog.txt`.
- [exec.c](../exec.c): after a successful exec, authorization is set only when
  the loaded inode is the recorded trusted `effectctl` inode. Successful exec
  of any other program clears the flag.
- [proc.c](../proc.c) and [proc.h](../proc.h): a newly allocated process starts
  unauthorized. `fork()` does not copy the capability, so a child must itself
  successfully exec `/effectctl` before calling `effect()`. This prevents
  accidental inheritance to unrelated child code. Process reuse also starts
  unauthorized.
- [sysfile.c](../sysfile.c): `sys_effect()` rejects unauthorized callers,
  disallowed paths, invalid operations, and invalid payloads before EPT
  lookup/insertion. CREATE requires zero payload bytes. APPEND requires 1–20
  bytes of valid UTF-8 with no NUL. The exact path spelling is required;
  aliases such as repeated separators, `.` and `..` are rejected, not
  canonicalized.
- [file.c](../file.c) and [sysfile.c](../sysfile.c): the trusted `/effectctl`
  inode is protected from writes, unlink, and hard-link creation.
- [security.c](../security.c), [fs.c](../fs.c), and [sysfile.c](../sysfile.c):
  the existing shared boot-context taint policy remains in place. Untrusted
  reads taint the context; writes through the filesystem path downgrade
  trusted regular files; tainted unlink/link restrictions remain enforced.
  Authorization changes do not bypass or reset taint.
- [effectctl.c](../effectctl.c): this user program is a syscall transport
  utility. It accepts a larger transport buffer so oversized requests reach
  kernel validation rather than being silently accepted or hidden by the CLI.
- [effect_test.c](../effect_test.c),
  [effect_auth_test.c](../effect_auth_test.c), and
  [untrusted_effect_auth_test.c](../untrusted_effect_auth_test.c) cover EPT,
  authorization, path, payload, and direct-syscall behavior.

Host-side checks in [tools.py](./tools.py) remain defense in depth. They are
not the kernel security boundary.

## EPT, WAL, and recovery architecture

The EPT layout was not changed: 16 blocks × 32 entries per block = 512
key/result entries. Each entry is 16 bytes (`uint64 key`, `uint64 result`).
Lookup hashes the low 32 bits of the key modulo capacity and linearly probes
with wraparound until it finds the key or an empty slot. The EPT has no
reclamation policy or action/payload fingerprint.

For a new key, `sys_effect()` performs the filesystem mutation and EPT result
insertion in the same xv6 transaction. A duplicate key returns the stored
result without repeating the mutation. The existing WAL records the changed
data, inode, and EPT blocks; recovery installs committed log blocks and clears
the log header. The root directory size fix in [mkfs.c](../mkfs.c) remains
`off = ((off + BSIZE - 1) / BSIZE) * BSIZE;`.

The host journal in [agent.py](./agent.py) is a separate persistence layer:

- PREPARED and RUNNING snapshots contain the exact action, arguments, task and
  step identity, and stable nonzero effect key. Each JSONL append is flushed
  and fsynced; creation and compaction also synchronize the containing
  directory.
- A torn unterminated final record is discarded; malformed complete records
  raise a corruption error. A syntactically complete, valid unterminated final
  record is retained and repaired with a newline so a committed pending task
  is not lost.
- Recovery selects incomplete tasks and reuses the saved action/key. It does
  not ask the planner to regenerate pending work. Recovered actions still pass
  `validate_action()` before execution. The recovery manager resumes at the
  first incomplete step, or after the last completed step; it does not replay
  completed steps.
- A successful guest result is journaled as complete only after the bridge
  reports success. Completed tasks are excluded from automatic recovery.
- The journal file is on the host filesystem, not in `fs.img`. It survives
  process restarts when the same persistent path is reused. It assumes one
  writer at a time; it does not provide interprocess locking.
- The normal [main.py](./main.py) entry point enables journaling by default
  under `$XDG_STATE_HOME/xv6-agent/<workspace-id>/tasks.jsonl`, falling back
  to `~/.local/state/xv6-agent/<workspace-id>/tasks.jsonl`. The workspace ID
  is derived from the canonical configured workspace path and persisted with
  each task. Pending tasks without the matching ID are rejected. The default
  location assumes the user's state directory is on persistent host storage.
  Other `Agent` callers can still opt out by omitting `journal_path`, so those
  configurations have no host-crash recovery guarantee.

## Verification executed

The current-source guest suites below ran in disposable image copies after the
capability-inheritance fix. The real VM demo also builds a disposable copy and
uses `cache=directsync` for the filesystem drive.

| Test | Actual result |
|---|---|
| `python3 -m unittest discover -s agent_lab -p 'test*.py' -v` | 25 tests, `OK`, exit 0 |
| Default startup/recovery tests | Default path and workspace isolation verified; pending task replay precedes new work; completed tasks are not replayed; repeated restarts preserve the original key |
| `python3 agent_lab/main.py mock` with temporary `XDG_STATE_HOME` | Exit 0; created one journal under the stable workspace-specific state path |
| `make -B fs.img xv6.img` in a disposable copy | Exit 0 |
| Guest `effect_test` | All 10 functional tests passed |
| Guest `effect_auth_test` | Direct create/append denied; noncanonical and disallowed paths denied; `/important.txt` unchanged; `/effectctl` protected |
| Guest `untrusted_effect_auth_test` | Direct create and append denied |
| Unauthorized duplicate replay | With `/agentlog.txt` at `AB`, direct replay of an existing authorized key was denied; file remained `AB` |
| Guest `security_test` | `security_test: 0 failure(s)` |
| `python3 agent_lab/real_qemu_host_crash_demo.py` | Host child exited 91 after commit; QEMU remained live; restart recovered key `2863447316644830283`; final content `AB`; journal completed |
| `python3 agent_lab/real_vm_crash_recovery_demo.py` | QEMU PID 219797 received SIGKILL; PID 219818 booted the identical image; content `AB` before crash, after reboot, and after replay; key `8596329505001953594` returned saved result `1`; WAL header clean; journal completed |
| Fresh disposable `bash crash_test.sh sweep` after `make clean` | `Summary: 18 / 18 crash points passed.` |
| `git diff --check` | Exit 0 after final source and report updates |

One initial crash-sweep setup failed before any crash point: its copied tree
contained an old `mkfs` executable, so the rebuilt image was only 1,000 blocks
and image creation failed with `read: Success`. The rerun excluded copied
build products, ran `make clean`, and then passed all 18 points. This was a
test-fixture issue, not a change to the crash policy or assertions. An initial
unauthorized-replay fixture attempt used malformed non-16-digit keys and
effectctl printed its usage; a corrected fixture using valid 16-digit keys
passed, with the file remaining `AB`.

### Real VM crash evidence

The latest run used this filesystem image for both boots:

```text
/tmp/xv6-real-vm-crash-rlkjm3de/repo/fs.img
inode=237026 size=1024000
initial sha256=16f2fb08ebd8360ee5f3f6e73385f6a581674e58a14bca4cd7d6f04707f4c584
after effect / after SIGKILL / after reboot / after replay:
  9a54be0fea70d39c235885108813a74bf20047d2e91103fc3a6d67c2e56f3b40
```

The first QEMU process was PID `219797`; it exited from SIGKILL before the
second QEMU process (PID `219818`) started. Both commands used the same
filesystem drive at index 1 and kernel drive at index 0:

```text
/usr/bin/qemu-system-i386 -nographic -monitor unix:/tmp/xv6-monitor-6b3fc69467a5432189b6a40e4f627595.sock,server,nowait -drive file=/tmp/xv6-real-vm-crash-rlkjm3de/repo/fs.img,index=1,media=disk,format=raw,cache=directsync -drive file=/tmp/xv6-real-vm-crash-rlkjm3de/repo/xv6.img,index=0,media=disk,format=raw,cache=unsafe -smp 2 -m 512
/usr/bin/qemu-system-i386 -nographic -monitor unix:/tmp/xv6-monitor-dbe5e82fc2ac472eb1aa20247fd647b8.sock,server,nowait -drive file=/tmp/xv6-real-vm-crash-rlkjm3de/repo/fs.img,index=1,media=disk,format=raw,cache=directsync -drive file=/tmp/xv6-real-vm-crash-rlkjm3de/repo/xv6.img,index=0,media=disk,format=raw,cache=unsafe -smp 2 -m 512
```

The run printed:

```text
HOST_JOURNAL_PREPARED task_id=c173ee493b8a41058fbd670d8c5ef0bb step_id=0 effect_key=8596329505001953594
GUEST_EFFECT_COMMITTED
CONTENT_BEFORE_CRASH=AB
QEMU_KILLED_WITH_SIGKILL
FIRST_QEMU_EXITED
SAME_FS_IMAGE_REUSED
SECOND_QEMU_STARTED pid=219818
CONTENT_AFTER_REBOOT_BEFORE_REPLAY=AB
RAW_EPT_BEFORE_REPLAY key=8596329505001953594 saved_result=1 WAL_header_n=0
ORIGINAL_EFFECT_KEY_RECOVERED=8596329505001953594
AGENT_RESTARTED
DUPLICATE_EFFECT_REPLAYED
FINAL_CONTENT=AB
JOURNAL_COMPLETED
RAW_METADATA_OK root_size=1024 root_blocks=[75, 1024] file_inode=35 directory_block=1024 EPT_result=1 WAL_header_n=0
REAL_VM_CRASH_RECOVERY_PASS
```

The content assertions, not the hash change, establish persistence. The hashes
support that the same image inode and bytes were used across both boots.

## Limitations and security assumptions

1. All guest processes that successfully execute the trusted `/effectctl`
   inode share the global capability and EPT namespace. This is not
   host-agent-only authentication or isolation between mutually untrusted
   `effectctl` callers.
2. The EPT stores only key/result. If an authorized caller reuses a key with
   different arguments, the saved result is returned; the host journal must
   preserve the original action/key pair.
3. xv6 WAL group commit is not a universal per-syscall durability barrier:
   when other filesystem operations remain outstanding, `end_op()` can return
   before the shared transaction is committed. The demonstrations establish
   persistence for the tested serialized effect workload, not arbitrary
   concurrent power-loss timing.
4. The journal requires persistent host storage to survive host reboot and
   currently assumes a single writer. The normal `main.py` entry point now
   enables it by default, but other `Agent(..., journal_path=None)`
   configurations remain non-recoverable after host process termination.
5. This audit does not claim that every Python AI policy or confirmation
   behavior is implemented in the guest kernel.

## Reproduction

From the repository root:

```sh
python3 -m unittest discover -s agent_lab -p 'test*.py' -v
python3 agent_lab/real_qemu_host_crash_demo.py
python3 agent_lab/real_vm_crash_recovery_demo.py
```

Run the crash sweep only in a fresh disposable repository copy, first running
`make clean` there to prevent copied build products or a stale
`fs.img.clean` from being reused, then run:

```sh
bash crash_test.sh sweep
```
