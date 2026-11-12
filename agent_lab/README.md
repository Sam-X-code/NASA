# xv6 AI agent and kernel security

The agent runs deterministic demos by default. `MockBridge` is useful for host-side
validation but has no kernel policy. The QEMU bridge keeps one guest shell alive;
its `cat` and later `rm` commands execute in different child processes.

The normal [main.py](./main.py) entry point enables the durable task journal by
default. It stores one journal per canonical agent workspace at
`$XDG_STATE_HOME/xv6-agent/<workspace-id>/tasks.jsonl`, or
`~/.local/state/xv6-agent/<workspace-id>/tasks.jsonl` when `XDG_STATE_HOME` is
unset. Startup recovers an unfinished task before running new work. The
workspace ID is a SHA-256 digest of the canonical workspace path; pending
records with a different or missing workspace identity are rejected. The
journal is host-side and must remain on persistent host storage to survive a
host restart.

Journal use remains configurable in tests and other callers with
`Agent(..., journal_path=...)`; when a custom path is used, pass a stable
`workspace_id` as well if tasks must be isolated between workspaces. An
unterminated final record that is valid JSON is retained and repaired with a
newline; an invalid torn final fragment is discarded, while malformed
newline-terminated records stop recovery with an error.

## Trust and taint policy

`mkfs` stores regular-file trust in the unused `major` field of a regular inode;
the inode's on-disk size and packing stay unchanged. Image inputs are untrusted
by default; `-t` and `-u` select the classification for subsequent inputs. The
Makefile explicitly marks `inbox.txt` untrusted and the other fixtures trusted.
Kernel-created regular files default to untrusted.
Legacy images whose regular-file `major` field is zero are treated as untrusted.

A non-empty user read from an untrusted regular file sets a locked, per-boot
sticky taint bit. Reads made internally by the filesystem and executable loader
do not set it. A tainted write downgrades any trusted regular-file destination.
`sys_unlink()` rejects deletion while the shared per-boot bit is set. The bit
survives `fork()` and `exec()` and resets only on reboot; there is no user reset
operation. The initial prototype blocks all unlink calls after taint, not just
deletion of a selected protected file.

The optional Ollama assessor labels successful read observations
`REVIEWED_SAFE`, `SUSPICIOUS`, or `UNCERTAIN` and logs the result. It has no tool
execution capability and cannot set kernel trust. If unavailable, the
assessment is recorded as `UNCERTAIN`; the kernel policy remains authoritative.

## Tests

Back up any valuable `fs.img` before rebuilding it: the changed Makefile image
recipe recreates it. Then run the direct kernel test:

```sh
python3 agent_lab/kernel_test.py
```

The test rebuilds a clean image, runs `security_test` in QEMU, and checks startup
state, trusted/zero/failed reads, taint after a fork/exec reader exits, tainted
write propagation, and unlink denial.

Run the host-side agent tests and deterministic mock demo with:

```sh
cd agent_lab
python3 -m unittest test_agent.py
python3 main.py mock
```

Run the persistent QEMU agent demo with Python confirmation bypassed for the
second injected delete attempt:

```sh
python3 agent_lab/main.py qemu /absolute/path/to/xv6-public
```

The optional LLM integrations require a local Ollama service:

```sh
python3 agent_lab/main.py qemu /absolute/path/to/xv6-public --assess --ollama
```
