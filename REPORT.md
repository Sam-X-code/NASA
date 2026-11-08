# Exactly-Once Filesystem Modifications in xv6: Final Project Report

---

## 1. Executive Summary

This project implements an atomic, crash-consistent, and idempotent filesystem modification syscall—`effect()`—within the **MIT xv6** operating system (x86 32-bit).

In standard POSIX systems, operations such as `write()` and `creat()` are non-idempotent. In distributed systems or fault-tolerant kernels, if a client crashes or a network partition occurs after an operation has been executed but before the response is acknowledged, retrying the request risks duplicate mutations (e.g., duplicate appends or overwrites).

By coupling a **persistent on-disk effect table** directly with xv6's **Write-Ahead Logging (WAL)** subsystem, this project guarantees **exactly-once execution semantics**:
- Every operation is identified by an immutable 64-bit client-supplied `key`.
- File mutations and the corresponding `(key, result)` record are committed in the **same atomic WAL transaction**.
- Across any crash point—whether during staging, writing to the log area, before or after the commit header, or during home block installation—a reboot and client replay yields **strictly consistent state** (e.g., initial `"A"` + append `"B"` $\rightarrow$ strictly `"AB"`, never `"A"` and never `"ABB"`).

---

## 2. Architecture & Implementation Breakdown

```
+-----------------------------------------------------------------------------------+
|                               USER SPACE                                          |
|                                                                                   |
|    effect(key, op, path, data, n)                crashtest / effect_test          |
+-----------------------------------------------------------------------------------+
                                       |
                                       v  Syscall Trap (SYS_effect, SYS_crashat)
+-----------------------------------------------------------------------------------+
|                               KERNEL SPACE                                        |
|                                                                                   |
|  [sysfile.c] sys_effect()                                                         |
|    |                                                                              |
|    +--> effect_tx_begin() [Acquires effect_lock (sleeplock), calls begin_op()]    |
|    |                                                                              |
|    +--> effect_tx_lookup(key)                                                     |
|    |      |-- If found: returns saved result immediately (IDEMPOTENT HIT)         |
|    |                                                                              |
|    +--> Filesystem Mutation:                                                      |
|    |      |-- EFFECT_CREATE: calls create()                                       |
|    |      |-- EFFECT_APPEND: calls writei()                                       |
|    |                                                                              |
|    +--> effect_tx_insert(key, result)                                             |
|    |      |-- Writes to effect table buffer via log_write()                       |
|    |                                                                              |
|    +--> effect_tx_end() [Calls end_op() -> commit(), releases effect_lock]        |
+-----------------------------------------------------------------------------------+
                                       |
                                       v
+-----------------------------------------------------------------------------------+
|                       WRITE-AHEAD LOGGING (log.c)                                 |
|                                                                                   |
|  commit():                                                                        |
|    1. write_log():     Writes modified cache blocks to on-disk log area           |
|    2. write_head():    ATOMIC COMMIT POINT (writes log header n > 0 to sector 2)  |
|    3. install_trans(): Installs blocks from log to home disk locations            |
|    4. write_head():    Clears log header (writes n = 0 to sector 2)               |
+-----------------------------------------------------------------------------------+
                                       |
                                       v
+-----------------------------------------------------------------------------------+
|                             ON-DISK LAYOUT (fs.img)                               |
|                                                                                   |
| [Sector 0] Boot block                                                             |
| [Sector 1] Superblock (ninodes, nblocks, logstart=2, effstart=59, effblocks=10)   |
| [Sectors 2..31] Log blocks (Sector 2 = Log Header; Sectors 3..31 = Log Data)      |
| [Sectors 32..57] Inode blocks                                                     |
| [Sector 58] Bitmap block                                                          |
| [Sectors 59..68] Persistent Effect Table (10 blocks = 320 entries)                |
| [Sectors 69..999] Data blocks                                                     |
+-----------------------------------------------------------------------------------+
```

---

## 3. Persistent Effect Table Specification

### 3.1 Superblock Integration & Entry Format
The superblock (`struct superblock` in [`fs.h`](file:///c:/Users/sacha/NASA/fs.h)) was extended with two fields:
- `uint effstart`: The starting block number for the effect table (sector 59).
- `uint effblocks`: The number of blocks allocated for the table (10 blocks).

Each effect table entry is 16 bytes:
```c
struct effect_entry {
  unsigned long long key;    // 8 bytes: 64-bit unique client token (0 = unused slot)
  unsigned long long result; // 8 bytes: saved return value (e.g. bytes written or 0)
};
```
With block size `BSIZE = 512`, each disk block stores:
$$\text{Entries per block} = \frac{512}{16} = 32 \text{ entries}$$
For 10 blocks, the table holds $10 \times 32 = 320$ persistent entries.

### 3.2 Hash Indexing & Open Addressing
Lookups and insertions in [`effect.c`](file:///c:/Users/sacha/NASA/effect.c) use an open-addressing hash table with linear probing:
- $\text{Start index} = (\text{uint})\text{key} \pmod{\text{capacity}}$.
- Probe sequence: $(\text{start} + i) \pmod{\text{capacity}}$ for $i \in [0, \text{capacity}-1]$.
- Unused slots are identified by `key == 0`.
- All writes to effect-table blocks are performed via `log_write(bp)`, ensuring they are strictly governed by the WAL.

---

## 4. Phase-by-Phase Implementation Summary

| Phase | Description | Deliverables & Verification |
|---|---|---|
| **Phase 1** | Understand WAL mechanics & transaction lifecycle | Analyzed `log.c`, `begin_op()`, `commit()`, `install_trans()`, `write_head()`. |
| **Phase 2** | Persistent Effect Table | Extended `mkfs.c`, `fs.h`, `effect.h`, `effect.c`. Disk formatting initializes empty table. |
| **Phase 3** | `effect()` System Call | Added `SYS_effect` (syscall 23). Handles 64-bit keys on 32-bit x86 via `(keyhi << 32) \| keylo`. |
| **Phase 4** | Atomic Transaction Boundaries & Concurrency | `effect_lock` held across `lookup -> mutation -> insert -> commit`. Prevents TOCTOU races. |
| **Phase 5** | Non-Crash Functional Tests | [`effect_test.c`](file:///c:/Users/sacha/NASA/effect_test.c): Tested new keys, duplicates, missing files, CREATE, empty payloads, concurrency. |
| **Phase 6** | `crashat(n)` Crash Injection | Added `SYS_crashat` (syscall 24) in `log.c`. Injects panics across all WAL stages. |
| **Phase 7** | Automated Crash/Reboot Harness | Built [`crash_test.sh`](file:///c:/Users/sacha/NASA/crash_test.sh) with QEMU automation, FIFO input, and state verification. |
| **Phase 8** | Crash Point Sweep | Executed sweep across 15 crash points (1–13, 101, 102). 100% passed (`AB`, len=2). |
| **Phase 9** | Write Quantification & Commit Argument | Measured disk write amplification and proved commit point atomicity. |

---

## 5. The Commit-Point Argument (Formal Proof)

A central requirement of write-ahead logging is establishing the exact instant an operation transitions from *uncommitted* to *committed*.

### 5.1 Why `write_head()` is the Exact Commit Point
In xv6, `write_head()` writes `struct logheader` to sector 2 on disk:
```c
struct logheader {
  int n;
  int block[LOGSIZE];
};
```
At the hardware layer, IDE/SATA controllers guarantee that writing a single 512-byte sector is **atomic** (either the sector completes or it does not; partial sector writes are detected by CRC/ECC errors).

```
                      PRE-COMMIT PHASE                          COMMIT POINT                       POST-COMMIT PHASE
          [Staging] ------------------> [write_log()] --------> |write_head()| --------> [install_trans()] ------> [write_head(0)]
On-Disk:  Log data: unwritten/stale     Log data: written       Header: n=3              Home blocks: written       Header: n=0
Status:   UNCOMMITTED                   UNCOMMITTED             COMMITTED                COMMITTED                  COMMITTED
Crash:    Reboot ignores log            Reboot ignores log      Reboot replays log       Reboot replays log         Reboot does nothing
Replay:   Executes append               Executes append         Skips (table cached)     Skips (table cached)       Skips (table cached)
```

### 5.2 Crash Before `write_head()` ($n = 0$ on disk)
- **State on Disk**: The log header sector retains $n = 0$ from the previous transaction. Even if some or all data blocks have been written to sectors 3..31 by `write_log()`, they are completely unreferenced. Home blocks (the file and effect table) have not been touched.
- **Recovery on Reboot**: `initlog()` calls `recover_from_log()`. `read_head()` reads $n = 0$. `install_trans()` executes 0 iterations. No blocks are copied.
- **Replay Behavior**: The client replays `effect(key, ...)`. The key is absent from the effect table. The operation executes normally, appending `"B"` and inserting `(key, result)`.
- **Result**: Exactly `"AB"` ($len = 2$).

### 5.3 Crash After `write_head()` ($n > 0$ on disk)
- **State on Disk**: Sector 2 contains $n = 3$ and block addresses `[data, inode, effect]`. All 3 blocks are durable in the log area. Even if the machine crashes before a single home block is copied by `install_trans()`, the transaction is durable.
- **Recovery on Reboot**: `initlog()` calls `recover_from_log()`. `read_head()` reads $n = 3$. `install_trans()` copies all 3 blocks from the log area into their respective home sectors (updating `crash_file.txt` and the persistent effect table). `write_head()` then clears the log ($n = 0$).
- **Replay Behavior**: The client replays `effect(key, ...)`. `effect_tx_lookup(key)` finds the entry in the persistent effect table and immediately returns `saved = 1` without repeating the append.
- **Result**: Exactly `"AB"` ($len = 2$). Never `"ABB"`.

---

## 6. Quantifying Disk Writes

### 6.1 Mathematical Formulation of xv6 WAL Writes
In xv6, any transaction logging $K$ distinct disk blocks undergoes the following physical writes during `commit()`:
1. **Log Data Writes**: $K$ disk writes in `write_log()` to copy blocks to the log area.
2. **Commit Header Write**: $1$ disk write in `write_head()` to write $n = K$ to disk (the commit point).
3. **Home Block Writes**: $K$ disk writes in `install_trans()` to copy blocks to their home locations.
4. **Log Clear Write**: $1$ disk write in `write_head()` to write $n = 0$ to disk.

Thus, the total number of physical disk writes $W(K)$ as a function of logged blocks $K$ is:
$$W(K) = K + 1 + K + 1 = 2K + 2$$

### 6.2 Comparison: Normal Append vs. Effect Append

| Metric | Normal Append (`write`) | Effect Append (`effect`) | Delta ($\Delta$) |
|---|---|---|---|
| **Blocks Logged ($K$)** | **2** (Data block + Inode block) | **3** (Data block + Inode + Effect Table) | $+1$ block |
| **`write_log()` Writes** | 2 | 3 | $+1$ write |
| **Commit Header (`write_head`)** | 1 | 1 | $0$ writes |
| **`install_trans()` Writes** | 2 | 3 | $+1$ write |
| **Clear Header (`write_head`)** | 1 | 1 | $0$ writes |
| **Total Disk Writes ($W$)** | **6 writes** | **8 writes** | **$+2$ disk writes** |

$$\text{Write Amplification Overhead} = \frac{8 - 6}{6} = 33.3\%$$

### 6.3 Cost of Idempotent Replays
When a client resubmits a previously completed request:
- `effect_tx_lookup()` finds the key in the buffer cache (or reads 1 block from disk if uncached).
- The transaction terminates immediately without calling `writei()` or `effect_tx_insert()`.
- **Physical disk writes on replay = 0**.

---

## 7. Crash Recovery Verification Results (Phase 8 Sweep)

The automated test harness [`crash_test.sh`](file:///c:/Users/sacha/NASA/crash_test.sh) swept all 15 crash points. In every trial:
- The initial file contained `"A"` ($len = 1$).
- Crash was triggered during an append of `"B"`.
- The system rebooted on the dirty image, replayed the exact same request, and inspected file contents.

```
================================================================================================
                           XV6 CRASH RECOVERY SWEEP REPORT
================================================================================================
Initial State: crash_file.txt containing 'A'
Operation:     effect(0xCAFE0000ULL + point, EFFECT_APPEND, 'crash_file.txt', 'B', 1)
Expected:      Strictly 'AB' (len=2) for all crash points
------------------------------------------------------------------------------------------------
POINT  | PHASE        | RESULT   | FINAL CONTENT | RET   | CRASH LOCATION
------------------------------------------------------------------------------------------------
1      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | log_write 1 (staging data block in memory)
2      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | log_write 2 (staging inode block in memory)
3      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | log_write 3 (staging effect-table block in memory)
4      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | write_log 1 (writing data block to disk log)
5      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | write_log 2 (writing inode block to disk log)
6      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | write_log 3 (writing effect block to disk log)
7      | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | commit: before write_head (PRE-COMMIT point)
8      | POST-COMMIT  | PASS     | AB (len=2)    | 1     | commit: after write_head (POST-COMMIT / COMMIT POINT)
9      | POST-COMMIT  | PASS     | AB (len=2)    | 1     | install_trans 1 (writing home data block)
10     | POST-COMMIT  | PASS     | AB (len=2)    | 1     | install_trans 2 (writing home inode block)
11     | POST-COMMIT  | PASS     | AB (len=2)    | 1     | install_trans 3 (writing home effect-table block)
12     | POST-COMMIT  | PASS     | AB (len=2)    | 1     | commit: after install_trans, before log clear
13     | POST-COMMIT  | PASS     | AB (len=2)    | 1     | commit: after log clear write_head
101    | PRE-COMMIT   | PASS     | AB (len=2)    | 1     | commit: before write_head (PRE-COMMIT point)
102    | POST-COMMIT  | PASS     | AB (len=2)    | 1     | commit: after write_head (POST-COMMIT / COMMIT POINT)
------------------------------------------------------------------------------------------------
Summary: 15 / 15 crash points passed.
ALL CRASH POINTS PASSED! Exactly-once semantics verified across crashes.
================================================================================================
```

---

## 8. Comprehensive Viva Examination Q&A

### Q1: Why must the effect table update occur in the SAME transaction as the file operation?
**Answer**: If they were in separate transactions, a crash could occur between them:
- If file mutation commits first and the system crashes before the effect table commits, the client replaying the request will perform the mutation a second time ($\rightarrow$ `"ABB"`, duplicate write).
- If the effect table commits first and the system crashes before the file mutation commits, the client replaying will believe the operation succeeded, but the file was never modified ($\rightarrow$ `"A"`, lost write).
Atomic inclusion in the same transaction guarantees that both or neither take effect.

### Q2: How are 64-bit keys handled on 32-bit x86 xv6?
**Answer**: In 32-bit x86 xv6, `argint()` retrieves 32-bit words from the trap frame. A 64-bit key occupies two 32-bit stack slots (`keylo` at argument 0 and `keyhi` at argument 1). The kernel reconstructs the full 64-bit integer using:
```c
key = ((unsigned long long)(uint)keyhi << 32) | (uint)keylo;
```
User-space prototypes in [`user.h`](file:///c:/Users/sacha/NASA/user.h) pass `unsigned long long key` directly, which the compiler naturally splits across two 32-bit registers/stack slots.

### Q3: What happens if a client submits an existing key with different parameters (different file or payload)?
**Answer**: In distributed systems and idempotent RPC theory, the key represents an **immutable request identity token**. Once key $K$ has executed and committed, any subsequent invocation with key $K$ is an idempotent duplicate. It returns the previously recorded return code without re-executing, regardless of whether the arguments differ. This was validated in `effect_test.c` (Test 6).

### Q4: How does the system handle concurrent calls with the same key?
**Answer**: A time-of-check to time-of-use (TOCTOU) race could occur if process 1 looks up key $K$, finds nothing, and begins appending, while process 2 simultaneously looks up key $K$ before process 1 commits.
To prevent this, `effect_tx_begin()` acquires a dedicated `effect_lock` (sleeplock) before calling `begin_op()`. This lock is held continuously across `lookup -> mutation -> insert -> commit()`. Process 2 is blocked until Process 1 has fully committed, at which point Process 2's lookup immediately discovers the entry and returns the cached result.

### Q5: Why is `write_head()` considered the single atomic commit point?
**Answer**: `write_head()` writes the 512-byte `logheader` to sector 2. Disk sector writes are atomic at the drive controller boundary. If sector 2 contains $n > 0$, the recovery algorithm on boot treats the entire set of logged blocks as committed and reinstalls them. If sector 2 contains $n = 0$, recovery ignores the log area. Thus, the completion of this single sector write irrevocably commits the transaction.

### Q6: What is the write overhead of adding the effect table to the WAL?
**Answer**: Exactly **2 additional disk writes** per transaction. A normal append writes 6 sectors (2 log + 1 header + 2 home + 1 clear header). An effect append writes 8 sectors (3 log + 1 header + 3 home + 1 clear header). When duplicate requests are replayed, the overhead is **0 writes** because the kernel returns the cached result without opening a WAL transaction.
