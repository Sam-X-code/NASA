# Final Release Verification Report

## Status

**The tested serialized recovery workflow passes; no files are staged or
committed.** The normal `main.py` entry point now enables durable journaling
by default and recovers pending workspace tasks before running new scripted
work. The underlying `Agent` API remains configurable; callers that omit a
journal path do not have crash-recovery guarantees.

This is the current verification report. Detailed kernel policy, source
behavior, real-VM command lines, and the test evidence are in
[KERNEL_AUTHORIZATION_REPORT.md](./KERNEL_AUTHORIZATION_REPORT.md).

## Architecture

- **Kernel security:** successful execution of the protected `/effectctl`
  inode grants a process-local capability. Ordinary direct syscall callers
  are denied. Fork does not copy the capability; successful exec resets it
  unless the executable is the trusted inode. The kernel independently
  enforces the exact paths `/report.txt`, `/inbox.txt`, `/important.txt`, and
  `/agentlog.txt`, operation-specific payload shape, UTF-8 validity, NUL
  rejection, and the append-size limit. Existing trust/taint enforcement is
  retained.
- **Guest exactly-once layer:** the existing EPT stores global key/result
  pairs and is updated in the same WAL transaction as a filesystem mutation.
  A duplicate key returns its saved result without repeating the mutation.
  No new EPT or caller identity fields were added.
- **Host recovery layer:** the JSONL task journal fsyncs PREPARED/RUNNING
  snapshots containing the exact action and stable key before guest
  execution. Recovery retries the saved action/key, revalidates the action,
  and marks completion only after a successful result. `main.py` defaults to
  `$XDG_STATE_HOME/xv6-agent/<workspace-id>/tasks.jsonl`, or
  `~/.local/state/xv6-agent/<workspace-id>/tasks.jsonl`. The workspace ID is
  derived from the canonical workspace path and is checked in pending
  records. Startup uses the persisted planner identity only to continue a task
  after replaying its immutable pending action; it fails closed for unknown
  planner identities. The host journal remains distinct from the guest EPT.
- **Filesystem image:** the `mkfs.c` root-directory ceiling-alignment fix is
  preserved. QEMU maps `fs.img` at drive index 1, matching `ROOTDEV=1`.

## Files changed

Kernel policy, lifecycle, EPT tests, image composition, sizing, and
filesystem fix: [effect.c](../effect.c), [sysfile.c](../sysfile.c),
[exec.c](../exec.c), [proc.c](../proc.c), [proc.h](../proc.h),
[defs.h](../defs.h), [file.c](../file.c), [effectctl.c](../effectctl.c),
[effect_test.c](../effect_test.c), [effect_auth_test.c](../effect_auth_test.c),
[untrusted_effect_auth_test.c](../untrusted_effect_auth_test.c),
[crashtest.c](../crashtest.c), [Makefile](../Makefile),
[param.h](../param.h), [mkfs.c](../mkfs.c), and [crash_test.sh](../crash_test.sh).
Host journal, bridge, and policy changes are in [agent.py](./agent.py),
[bridge.py](./bridge.py), and [tools.py](./tools.py). New host coverage includes
[test_effect_bridge.py](./test_effect_bridge.py) and
[test_journal_recovery.py](./test_journal_recovery.py), and
[test_main_recovery.py](./test_main_recovery.py). Demonstrations include
[host_crash_recovery_demo.py](./host_crash_recovery_demo.py) (simulated),
[real_qemu_host_crash_demo.py](./real_qemu_host_crash_demo.py),
[real_vm_crash_recovery_demo.py](./real_vm_crash_recovery_demo.py), and
[recovery_demo.py](./recovery_demo.py).

## Executed tests

| Test | Result |
|---|---|
| `python3 -m unittest discover -s agent_lab -p 'test*.py' -v` | 25 tests, `OK`, exit 0 |
| Default application startup / recovery tests | Journal configured by default; pending recovery precedes new work; completed tasks are not replayed; workspace mismatch is rejected |
| `python3 agent_lab/main.py mock` with temporary `XDG_STATE_HOME` | Exit 0; created one journal at the workspace-hashed state path |
| Clean disposable `make -B fs.img xv6.img` | Exit 0 |
| Guest `effect_test` | 10/10 functional tests passed |
| Guest `effect_auth_test` | Direct syscall, path alias, policy, and protected-executable checks passed |
| Guest `untrusted_effect_auth_test` | Direct create and append denied |
| Unauthorized duplicate replay | Denied; `/agentlog.txt` remained `AB` |
| Guest `security_test` | `security_test: 0 failure(s)` |
| Real host-agent termination/recovery | Child exit 91; same QEMU stayed live; key `2863447316644830283` recovered; final content `AB`; journal completed |
| Real VM SIGKILL/reboot/recovery | QEMU PID 219797 was SIGKILLed; PID 219818 reused the same `fs.img` path/inode; content `AB` before crash, after reboot, and after replay; key `8596329505001953594` returned saved result `1`; WAL header clean; journal completed |
| Fresh disposable crash sweep | `Summary: 18 / 18 crash points passed.` |
| `git diff --check` | Passed after final report updates |

The first crash-sweep setup failed before testing because a copied stale `mkfs`
binary generated a 1,000-block image that could not contain the test image
contents. The disposable copy was then cleaned before the sweep; the rerun
rebuilt `mkfs` and passed all 18 crash points. No test assertions were
weakened. An initial unauthorized-replay fixture attempt also used malformed
non-16-digit keys; `effectctl` printed its usage and the expected `AB` fixture
was not created. The fixture was rerun with valid 16-digit keys, then the
unauthorized replay was denied and the file remained `AB`.

## Guarantees and limits

- The verified recovery guarantee is for the tested serialized operation:
  journal PREPARED → guest effect/EPT commit → host or VM crash → replay of
  the exact same action/key → final `AB`, never `ABB`.
- All guest processes that execute the trusted `effectctl` inode share the
  capability and EPT namespace. This is not host-agent-only authentication or
  isolation between effectctl callers.
- EPT entries contain no payload fingerprint. Correct replay depends on the
  host journal preserving the original operation and key.
- xv6 uses group commit. An effect overlapping other outstanding filesystem
  operations is not necessarily a per-syscall durability barrier when its
  syscall returns. The test evidence does not establish universal concurrent
  power-loss durability.
- The task journal assumes one writer process at a time and survives host
  reboot only when stored on persistent host storage.
- `main.py` enables journaling by default. Other `Agent` configurations that
  omit `journal_path`, or place the journal on nonpersistent storage, do not
  receive the same recovery guarantee.
- A complete unterminated JSON record is preserved and its newline repaired.
  An invalid incomplete final fragment is truncated as a torn write; malformed
  complete records and workspace mismatches stop recovery rather than being
  ignored.
- These results do not claim that every Python AI policy or confirmation
  behavior is enforced in the guest kernel.

## Reproduction

From the repository root:

```sh
python3 -m unittest discover -s agent_lab -p 'test*.py' -v
python3 agent_lab/main.py mock
python3 agent_lab/real_qemu_host_crash_demo.py
python3 agent_lab/real_vm_crash_recovery_demo.py
```

For the full crash sweep, use a fresh disposable repository copy, run
`make clean` in that copy, then execute:

```sh
bash crash_test.sh sweep
```

No files were staged, committed, pushed, or sent to a remote.
