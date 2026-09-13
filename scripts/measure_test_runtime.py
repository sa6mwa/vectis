#!/usr/bin/env python3
"""Measure a command in a fresh cgroup (run via systemd-run --user).

CPU includes descendants; memory.peak is the aggregate cgroup high-water mark,
including charged file cache and this small measurement process, not summed RSS.
"""
import json
import pathlib
import subprocess
import sys
import time


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: measure_test_runtime.py COMMAND [ARG ...]")
    membership = pathlib.Path("/proc/self/cgroup").read_text().strip()
    if not membership.startswith("0::/"):
        raise SystemExit("measurement requires cgroup v2")
    group = pathlib.Path("/sys/fs/cgroup") / membership[3:].lstrip("/")

    def cpu():
        return dict(line.split() for line in (group / "cpu.stat").read_text().splitlines())

    before = cpu()
    start = time.monotonic_ns()
    result = subprocess.run(sys.argv[1:], check=False)
    wall = time.monotonic_ns() - start
    after = cpu()
    print("RUNTIME_MEASUREMENT " + json.dumps({
        "command": sys.argv[1:],
        "exit_code": result.returncode,
        "wall_ns": wall,
        "cpu_user_us": int(after["user_usec"]) - int(before["user_usec"]),
        "cpu_system_us": int(after["system_usec"]) - int(before["system_usec"]),
        "cpu_total_us": int(after["usage_usec"]) - int(before["usage_usec"]),
        "cgroup_memory_peak_bytes": int((group / "memory.peak").read_text()),
        "cgroup_swap_peak_bytes": int((group / "memory.swap.peak").read_text()),
    }), flush=True)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
