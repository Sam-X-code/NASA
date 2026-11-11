# xv6 AI agent and kernel security

The agent runs deterministic demos by default. `MockBridge` is useful for host-side
validation but has no kernel policy. The QEMU bridge keeps one guest shell alive;
its `cat` and later `rm` commands execute in different child processes.

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
