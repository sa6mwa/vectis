#!/usr/bin/env python3
"""Check the Darwin test wrapper's exit and orphan cleanup contracts."""

import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

from run_forked_kore_test_darwin import group_members


FIXTURE = '''#!/usr/bin/env python3
import os
import signal
import time

pid = os.fork()
if pid == 0:
    os.setpgid(0, 0)
    with open("kore.pid", "w", encoding="ascii") as stream:
        stream.write(str(os.getpid()))
    with open(os.environ["PID_OUT"], "w", encoding="ascii") as stream:
        stream.write(str(os.getpid()))
    while True:
        signal.pause()
os.setpgid(pid, pid)
for _ in range(100):
    if os.path.isfile("kore.pid"):
        break
    time.sleep(0.01)
else:
    os._exit(72)
mode = os.environ["FIXTURE_MODE"]
if mode == "timeout":
    time.sleep(60)
os._exit(71 if mode == "failure" else 0)
'''


def check_case(root, mode, expected):
    fixture = root / "fixture"
    pid_out = root / "child.pid"
    fixture.write_text(FIXTURE, encoding="utf-8")
    fixture.chmod(0o755)
    env = os.environ.copy()
    env["FIXTURE_MODE"] = mode
    env["PID_OUT"] = str(pid_out)
    wrapper = Path(__file__).with_name("run_forked_kore_test_darwin.py")
    try:
        result = subprocess.run(
            [sys.executable, str(wrapper), "1" if mode == "timeout" else "3",
             str(expected), str(fixture)],
            cwd=root, env=env, text=True, capture_output=True, timeout=10,
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        group = int(pid_out.read_text(encoding="ascii"))
        assert not group_members(group), "Kore process group survived " + mode
    finally:
        if pid_out.exists():
            group = int(pid_out.read_text(encoding="ascii"))
            if group_members(group):
                os.killpg(group, signal.SIGKILL)


def main():
    with tempfile.TemporaryDirectory(prefix="vectis-kore-wrapper.", dir=os.getcwd()) as directory:
        root = Path(directory)
        for mode, expected in (("success", 0), ("failure", 71), ("timeout", 124)):
            check_case(root, mode, expected)


if __name__ == "__main__":
    main()
