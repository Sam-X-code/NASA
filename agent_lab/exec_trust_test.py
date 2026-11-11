import os
import subprocess

import pexpect


XV6_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_PASS = "security_test: 0 failure(s)"


def build_filesystem():
    subprocess.run(
        ["make", "-B", "EXEC_TRUST_TEST=1", "fs.img"],
        cwd=XV6_DIR, check=True, stdout=subprocess.DEVNULL)


def run_boot(mode, fixture_marker=None):
    child = pexpect.spawn(
        "make", ["EXEC_TRUST_TEST=1", "qemu-nox"],
        cwd=XV6_DIR, encoding="utf-8", timeout=180)
    try:
        child.expect(r"init: starting sh\r?\n\$ ")
        child.sendline(f"security_test {mode}")
        child.expect(r"\$ ", timeout=30)
        output = child.before.replace("\r", "")
        print(output)
        if TEST_PASS not in output:
            raise RuntimeError(f"{mode} executable-trust tests failed")
        if fixture_marker and fixture_marker not in output:
            raise RuntimeError(f"{mode} fixture did not execute")
        if "exec fixture failed:" in output:
            raise RuntimeError(f"{mode} executable was rejected")
    finally:
        child.terminate(force=True)


def main():
    subprocess.run(
        ["make", "-B", "EXEC_TRUST_TEST=1", "kernel", "xv6.img"],
        cwd=XV6_DIR, check=True, stdout=subprocess.DEVNULL)

    for mode, marker in (
        ("trusted", "trusted executable ran"),
        ("untrusted", "untrusted executable ran"),
        ("missing-context", None),
    ):
        build_filesystem()
        run_boot(mode, marker)


if __name__ == "__main__":
    main()
