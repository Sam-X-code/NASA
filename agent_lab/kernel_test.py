import os
import subprocess

import pexpect


XV6_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPECTED = (
    "fresh boot starts untainted: PASS",
    "trusted read succeeds without taint: PASS",
    "zero-byte untrusted read does not taint: PASS",
    "failed read does not taint: PASS",
    "fork/exec child read untrusted file: PASS",
    "tainted write downgrades trusted destination: PASS",
    "tainted process cannot unlink important.txt: PASS",
    "security_test: 0 failure(s)",
)


def main():
    subprocess.run(["make", "-B", "fs.img"], cwd=XV6_DIR, check=True)
    child = pexpect.spawn(
        "make qemu-nox", cwd=XV6_DIR, encoding="utf-8", timeout=180)
    try:
        child.expect(r"init: starting sh\r?\n\$ ")
        child.sendline("security_test")
        child.expect(r"\$ ", timeout=30)
        output = child.before.replace("\r", "")
        print(output)
        missing = [line for line in EXPECTED if line not in output]
        if missing:
            raise RuntimeError(f"kernel security tests failed or missing: {missing}")
    finally:
        child.terminate(force=True)

    rebooted = pexpect.spawn(
        "make qemu-nox", cwd=XV6_DIR, encoding="utf-8", timeout=180)
    try:
        rebooted.expect(r"init: starting sh\r?\n\$ ")
        rebooted.sendline("rm important.txt")
        rebooted.expect(r"\$ ", timeout=30)
        rebooted.sendline("cat important.txt")
        rebooted.expect(r"\$ ", timeout=30)
        output = rebooted.before.replace("\r", "")
        print(output)
        if "cat: cannot open important.txt" not in output:
            raise RuntimeError("taint did not reset after reboot")
        print("taint resets after reboot: PASS")
    finally:
        rebooted.terminate(force=True)


if __name__ == "__main__":
    main()
