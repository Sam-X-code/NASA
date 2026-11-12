# Exactly-Once Filesystem Mutations in xv6: Technical Design & Verification Report

---

## 1. Problem Statement & Motivation

In standard UNIX/POSIX filesystems, operations such as write() and creat() are non-idempotent. If a process, network link, or machine fails midway through an operation, standard recovery cannot determine whether a client request was executed before the crash:

- Retrying a failed append risks duplicating data (e.g., "AB" becomes "ABB").
- Aborting on failure risks data loss (e.g., append was lost, leaving "A").

This project designs and implements an atomic, crash-consistent, and idempotent filesystem modification system call, effect(), within the MIT xv6 operating system (32-bit x86). By integrating an on-disk persistent effect table directly into xv6 Write-Ahead Logging (WAL) subsystem, the system guarantees exactly-once execution semantics:

1. Every operation carries an immutable 64-bit client token (key).
2. The filesystem mutation and the corresponding (key, result) record are committed in the same atomic WAL transaction.
3. Replaying a request after a crash at any point restores or preserves the intended state without data loss or duplicate execution.

---

## 2. System Architecture

```
+-------------------------------------------------------------------------------+
|                                USER SPACE                                     |
|                                                                               |
|   effect(key, op, path, data, n)               crash_test.sh / crashtest      |
+-------------------------------------------------------------------------------+
                                      |
                                      v  Syscall Trap (SYS_effect, SYS_crashat)
+-------------------------------------------------------------------------------+
|                                KERNEL SPACE                                   |
|                                                                               |
|   sys_effect() [sysfile.c]                                                    |
|     |                                                                         |
|     +--> effect_tx_begin()                                                    |
|     |      - Acquires effect_lock (sleeplock)                                 |
|     |      - Calls begin_op() (allocates WAL log reservation)                 |
|     |                                                                         |
|     +--> effect_tx_lookup(key)                                                |
|     |      - Probes persistent table                                          |
|     |      - If found: returns saved result immediately [IDEMPOTENT HIT]      |
|     |                                                                         |
|     +--> Filesystem Mutation                                                  |
|     |      - EFFECT_CREATE: calls create()                                    |
|     |      - EFFECT_APPEND: calls writei()                                    |
|     |                                                                         |
|     +--> effect_tx_insert(key, result)                                        |
|     |      - Stages (key, result) via log_write()                             |
|     |                                                                         |
|     +--> effect_tx_end()                                                      |
|            - Calls end_op() -> commit()                                       |
|            - Releases effect_lock                                             |
+-------------------------------------------------------------------------------+
                                      |
                                      v
+-------------------------------------------------------------------------------+
|                          WRITE-AHEAD LOGGING (log.c)                          |
|                                                                               |
|   commit():                                                                   |
|     1. write_log():     Writes modified cache blocks to on-disk log area      |
|     2. write_head():    ATOMIC COMMIT POINT (writes log header n > 0)         |
|     3. install_trans(): Installs blocks from log to permanent home sectors    |
|     4. write_head():    Clears log header (writes n = 0)                      |
+-------------------------------------------------------------------------------+
                                      |
                                      v
+-------------------------------------------------------------------------------+
|                           ON-DISK LAYOUT (fs.img)                             |
|                                                                               |
|   Sector 0:       Boot Block                                                  |
|   Sector 1:       Superblock (effstart=59, effblocks=10)                      |
|   Sectors 2..31:  WAL Log (Sector 2 = Log Header; Sectors 3..31 = Log Blocks) |
|   Sectors 32..57: Inode Blocks                                                |
|   Sector 58:      Bitmap Block                                                |
|   Sectors 59..68: Persistent Effect Table (10 blocks = 320 entries)           |
|   Sectors 69..999: Data Blocks                                                |
+-------------------------------------------------------------------------------+
```

---

## 3. Persistent On-Disk Effect Table

### 3.1 Superblock Metadata & Table Sizing
The filesystem superblock (struct superblock in fs.h) was extended to record the table location:
- uint effstart: Starting sector of the table (sector 59).
- uint effblocks: Number of dedicated disk blocks (10 blocks).

Each table entry occupies 16 bytes:
```c
struct effect_entry {
  unsigned long long key;    // 8 bytes: unique 64-bit client token (0 = empty slot)
  unsigned long long result; // 8 bytes: saved return value (e.g. bytes written or 0)
};
```

Capacity calculation:
  Capacity = (512 bytes per block / 16 bytes per entry) × 10 blocks = 320 persistent entries

### 3.2 Indexing & Open Addressing
- Hash Function: start_index = (uint)key % 320
- Collision Resolution: Linear probing with modular wraparound
- WAL Safety: All modifications to table buffers are written via log_write(bp). Direct disk writes outside the log are strictly prohibited.

---

## 4. Formal Proof of the Commit Point

A fundamental question in WAL design is identifying the exact boundary where an operation transitions from uncommitted (aborted on crash) to committed (replayed on reboot).

```
          PRE-COMMIT PHASE                          COMMIT POINT                      POST-COMMIT PHASE
  [log_write] -------> [write_log] ---------> | write_head(n > 0) | ---------> [install_trans] -------> [write_head(0)]
   In-Memory           Log on Disk                 Sector 2 on Disk             Home Blocks on Disk      Log Cleared
  (Uncommitted)       (Uncommitted)                  (COMMITTED)                    (COMMITTED)          (COMMITTED)
```

### 4.1 Why write_head() is the Exact Commit Point
In xv6, write_head() writes struct logheader to sector 2:
```c
struct logheader {
  int n;
  int block[LOGSIZE];
};
```
At the hardware layer, IDE/SATA controllers guarantee that a single 512-byte sector write is atomic (it either completes fully or fails entirely; partial sector corruption is rejected by drive ECC).

### 4.2 Behavior Across Crash Scenarios

| Scenario | State on Disk at Crash | Recovery on Reboot | Client Replay Outcome | Final File Content |
|---|---|---|---|---|
| Crash Before write_head() (Points 1–7, 101) | Header has n = 0. Staged log blocks are unreferenced. Home blocks untouched. | read_head() reads n = 0. install_trans() does 0 iterations. Transaction is completely discarded. | Key is absent from table. effect() executes the mutation and records the result. | "AB" (len = 2) |
| Crash After write_head() (Points 8–13, 102) | Header has n = 3. All modified blocks are durable in the log area. | read_head() reads n = 3. install_trans() copies all 3 blocks to home sectors. Log is cleared. | Key is present in table. effect() finds cached result and skips re-execution. | "AB" (len = 2) |

Conclusion: Across every failure scenario, client re-execution guarantees exactly "AB" (len = 2), never "A" (lost write) and never "ABB" (duplicate write).

---

## 5. Disk Write Quantification & Overhead Analysis

### 5.1 General Mathematical Formulation
In xv6 write-ahead logging, every transaction modifying K distinct disk blocks performs:
1. K disk writes in write_log() (copying cached blocks to the log area).
2. 1 disk write in write_head() (writing the commit header with n = K).
3. K disk writes in install_trans() (copying blocks to their home locations).
4. 1 disk write in write_head() (writing the clear header with n = 0).

Formula:
  Total Disk Writes: W(K) = K + 1 + K + 1 = 2K + 2

### 5.2 Comparative Breakdown

| Operation Stage | Normal Append (write) | Effect Append (effect) | Overhead |
|---|:---:|:---:|:---:|
| Blocks Logged (K) | 2 (Data + Inode) | 3 (Data + Inode + Effect Table) | +1 block |
| write_log() disk writes | 2 | 3 | +1 write |
| Commit header write (write_head) | 1 | 1 | 0 writes |
| install_trans() home writes | 2 | 3 | +1 write |
| Log clear header write (write_head) | 1 | 1 | 0 writes |
| Total Physical Disk Writes: W(K) | 6 writes | 8 writes | +2 disk writes |

Write Amplification:
  Overhead = (8 - 6) / 6 = 33.3%

### 5.3 Idempotent Replay Cost
When an operation is replayed with a previously committed key:
- effect_tx_lookup() finds the key in the buffer cache.
- The system call returns the saved result immediately without opening a transaction.
- Physical disk writes on replay = 0.

---

## 6. Implementation Phases

| Phase | Milestone | Core Implementation |
|:---:|---|---|
| 1 | WAL Lifecycle Analysis | Detailed review of log.c, begin_op(), commit(), and install_trans(). |
| 2 | Persistent Effect Table | Superblock metadata in fs.h, disk layout in mkfs.c, table logic in effect.c. |
| 3 | effect() Syscall | sys_effect() in sysfile.c, 64-bit key marshaling via (keyhi << 32) \| keylo. |
| 4 | Atomic Boundaries & Concurrency | effect_lock held across lookup -> mutation -> insert -> commit to prevent TOCTOU races. |
| 5 | Non-Crash Functional Tests | Comprehensive test suite in effect_test.c (new key, duplicate, CREATE, missing files, concurrency). |
| 6 | Crash Injection (crashat) | Syscall in log.c triggering panics across log staging, header commit, and home installation. |
| 7 | Crash/Reboot Harness | Automated script crash_test.sh with FIFO input management and multi-boot coordination. |
| 8 | Automated Crash Sweep | Automated testing across all 15 crash points with full state verification. |
| 9 | Evaluation & Reporting | Mathematical write quantification, formal commit-point proof, and final documentation. |

---

## 7. Crash Recovery Sweep Results

The test harness (crash_test.sh) executed an automated sweep across all 15 crash injection points. In each trial, crash_file.txt was initialized with "A", an append of "B" was injected with a crash at the target point, the system rebooted on the dirty image, and the request was replayed.

| Point | Phase | Target Location | Outcome | Final File | Return Code | Verification |
|:---:|:---:|---|:---:|:---:|:---:|:---:|
| 1 | PRE-COMMIT | log_write 1: staging data block in memory | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 2 | PRE-COMMIT | log_write 2: staging inode block in memory | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 3 | PRE-COMMIT | log_write 3: staging effect-table block in memory | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 4 | PRE-COMMIT | write_log 1: writing data block to log area | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 5 | PRE-COMMIT | write_log 2: writing inode block to log area | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 6 | PRE-COMMIT | write_log 3: writing effect block to log area | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 7 | PRE-COMMIT | commit: before write_head (uncommitted) | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 8 | POST-COMMIT | commit: after write_head (COMMIT POINT) | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |
| 9 | POST-COMMIT | install_trans 1: writing home data block | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |
| 10 | POST-COMMIT | install_trans 2: writing home inode block | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |
| 11 | POST-COMMIT | install_trans 3: writing home effect block | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |
| 12 | POST-COMMIT | commit: after install, before log clear | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |
| 13 | POST-COMMIT | commit: after log clear write_head | PASS | AB (len = 2) | 1 | Durable on Disk -> Cached Return |
| 101 | PRE-COMMIT | Named Hook: immediate pre-commit | PASS | AB (len = 2) | 1 | Clean Rollback -> Replay Appended |
| 102 | POST-COMMIT | Named Hook: immediate post-commit | PASS | AB (len = 2) | 1 | WAL Redo Recovery -> Cached Return |

Summary: 15 / 15 crash points passed. Exactly-once semantics verified across all stages of transaction execution.

---

## 8. Challenges Faced & Engineering Solutions

Building an atomic, crash-consistent system call inside an OS kernel and verifying it across automated QEMU crash-reboot sweeps surfaced several subtle systems and tooling challenges:

### 8.1 Passing 64-Bit Keys Across a 32-Bit Syscall ABI
- Challenge: xv6 targets 32-bit x86, where argint() fetches 32-bit integers from the user stack frame. Because unsigned long long is 64 bits wide, passing key as the first parameter consumed two 32-bit stack slots (arguments 0 and 1), shifting all subsequent arguments (op, path, data, n) by one offset. Misaligned offsets caused invalid pointer and length reads inside sys_effect().
- Solution: Explicitly read the lower and upper 32-bit halves at argument indices 0 and 1, reconstructed the 64-bit key using ((unsigned long long)(uint)keyhi << 32) | (uint)keylo, and shifted op, path, data, and n to argument indices 2, 3, 4, and 5.

### 8.2 Split-Transaction Crash Windows & TOCTOU Concurrency Races
- Challenge: Early prototypes executed lookup, file mutation, and table insertion separately. This created three critical failure modes:
  1. Crash inconsistency: A crash between committing the file mutation and committing the effect entry would allow a replay to execute the append twice (producing "ABB").
  2. Time-of-check to time-of-use (TOCTOU) race: Two concurrent processes invoking effect() with the same key could both observe a table miss, execute the append simultaneously, and violate exactly-once semantics.
  3. Lock and transaction leaks: Returning early on validation failures (e.g., missing file or full table) risked leaving inode locks or open WAL transactions active.
- Solution: Wrapped the entire lookup -> mutation -> pre-flight space check -> insert -> commit sequence inside a single begin_op() / end_op() transaction guarded by a continuous sleeplock (effect_lock via effect_tx_begin() and effect_tx_end()). Ensured every error branch cleanly releases inode locks via iunlockput() before closing the transaction.

### 8.3 Persistent Effect Table Collisions Across Test Runs
- Challenge: Because the effect table persists on disk across reboots (sectors 59-68), re-running functional or crash tests against an existing fs.img caused confusing failures where files remained empty (len = 0) or unchanged (len = 1). Even after unlinking the target file, the old key remained saved in the on-disk effect table, causing subsequent effect() calls to hit the cache, return immediately, and skip both file mutations and WAL crash hooks. Additionally, running make fs.img inside crash_test.sh skipped rebuilding when fs.img was newer than source binaries, copying a dirty disk image into fs.img.clean.
- Solution:
  1. In effect_test.c, dynamically seeded test keys per execution using uptime() so repeated runs never collide with prior disk state.
  2. In crash_test.sh, explicitly deleted fs.img prior to make fs.img to guarantee a completely zeroed effect table in fs.img.clean.
  3. In crashtest.c, derived isolated per-point keys (0xCAFE0000ULL + point) shared identically between run and replay so crash points never cross-contaminate.

### 8.4 Freestanding User-Space Build Constraints
- Challenge: xv6 user-space binaries compile with -nostdinc and -fno-builtin, lacking standard C library headers such as string.h and functions like memcmp(). Referencing standard headers triggered -Werror compilation failures.
- Solution: Implemented a freestanding memcmp() helper directly inside effect_test.c to keep test binaries completely self-contained.

### 8.5 Dropped UART Keystrokes & Premature EOF in QEMU Automation
- Challenge: Automating 15 crash-and-reboot cycles in crash_test.sh revealed three automation and harness issues when piping commands into headless QEMU:
  1. Dropped characters during boot: Waiting a fixed 0.8 seconds before piping input occasionally sent characters while SeaBIOS or xv6 UART initialization was still resetting, dropping the leading 'c' of "crashtest replay" and causing "exec rashtest failed".
  2. Premature EOF: Exiting the piped subshell immediately after echo closed stdin, causing xv6 sh to read EOF in getcmd() and terminate before the command finished.
  3. Silent failure masking in harness: run_crash_cycle() always returned exit status 0, causing the summary counter to report 15 / 15 passed even when individual tests failed.
- Solution: Replaced static sleeps with a named pipe (FIFO) held open on file descriptor 3, polled QEMU serial output until the xv6 shell prompt ("$ ") appeared before transmitting the command, detected completion markers ("panic:", "CRASH:", "CRASH_TEST_RESULT:") to terminate QEMU promptly, and updated run_crash_cycle() to return 1 on verification failure.

### 8.6 Covering Both Sides of the Atomic Commit Point
- Challenge: Placing a single panic hook inside log_write() only tests uncommitted in-memory staging and fails to prove WAL recovery correctness after commit.
- Solution: Instrumented log.c with both an incrementing event counter (crash_current) and explicit named phase hooks (CRASH_COMMIT_BEFORE, CRASH_COMMIT_AFTER, CRASH_INSTALL_MID, CRASH_CLEAR_BEFORE, CRASH_CLEAR_AFTER) across log_write(), write_log(), write_head(), and install_trans().

### 8.7 Cross-Platform Windows / WSL Tooling & Line-Ending Issues
- Challenge: Editing files on Windows while compiling and running QEMU inside Ubuntu WSL introduced CRLF (\r\n) line endings and UTF-8 BOMs into build scripts (breaking sign.pl, vectors.pl, and bash execution of crash_test.sh), and generated ephemeral artifacts (crash_logs/, fs.img.clean, compiled user-program binaries) that interfered with Git tracking.
- Solution: Converted all build and test scripts to strict Unix LF endings without BOMs and updated .gitignore to exclude crash_logs/ and fs.img.clean.

---

## 9. Conclusion

By coupling an on-disk effect table directly with xv6 Write-Ahead Logging subsystem and holding an atomic lock boundary across lookup, file mutation, table insertion, and commit:
1. Atomicity: Either both the filesystem mutation and the effect record commit, or neither does.
2. Crash Consistency: The physical sector write in write_head() provides an unambiguous, atomic commit point.
3. Idempotence: Replaying requests after any system failure incurs zero duplicate writes and zero lost updates.
4. Efficiency: Guarantees are provided with an overhead of exactly 2 additional disk writes per transaction, and 0 writes on subsequent replays.
