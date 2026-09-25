#!/usr/bin/env python3
"""Run a forked Kore test and retire its isolated runtime process group."""

import os
import select
import signal
import subprocess
import sys
import tempfile
import time


def group_members(group):
    """Inspect only PIDs needed to prove ownership of the recorded group."""
    output = subprocess.check_output(
        ["ps", "-A", "-o", "pid=", "-o", "stat="], text=True
    )
    members = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[1].startswith("Z"):
            continue
        try:
            pid = int(fields[0])
            if os.getpgid(pid) == group:
                members.append((pid, os.getsid(pid)))
        except (OSError, ValueError):
            continue
    return members


def stop_group(group, session):
    members = group_members(group)
    if not members:
        return True
    if any(member_session != session for _, member_session in members):
        print("refusing to signal a Kore group outside this test session", file=sys.stderr)
        return False
    print("stopping owned Kore process group {}".format(group), file=sys.stderr)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(group, sig)
        except ProcessLookupError:
            return True
        for _ in range(50):
            time.sleep(0.02)
            if not group_members(group):
                return True
    print("owned Kore process group {} did not stop".format(group), file=sys.stderr)
    return False


def supervise(write_fd, executable, timeout_seconds, run_dir):
    os.setsid()
    os.chdir(run_dir)
    for key, name in (("ASAN_OPTIONS", "asan"), ("UBSAN_OPTIONS", "ubsan")):
        options = os.environ.get(key, "")
        os.environ[key] = options + (":" if options else "") + "log_path=" + run_dir + "/" + name
    child = os.fork()
    if child == 0:
        os.close(write_fd)
        os.execv(executable, [executable])
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        done, status = os.waitpid(child, os.WNOHANG)
        if done:
            result = os.waitstatus_to_exitcode(status)
            break
        time.sleep(0.02)
    else:
        try:
            os.kill(child, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            done, _ = os.waitpid(child, os.WNOHANG)
            if not done:
                time.sleep(0.1)
                done, _ = os.waitpid(child, os.WNOHANG)
            if not done:
                os.kill(child, signal.SIGKILL)
                os.waitpid(child, 0)
        except ProcessLookupError:
            pass
        result = 124
    os.write(write_fd, "{}\n".format(result).encode("ascii"))
    os.close(write_fd)
    while True:
        signal.pause()


def main():
    if len(sys.argv) != 4:
        print("usage: run_forked_kore_test_darwin.py timeout-seconds expected-exit executable", file=sys.stderr)
        return 2
    timeout_seconds = int(sys.argv[1])
    expected_exit = int(sys.argv[2])
    executable = os.path.realpath(sys.argv[3])
    if timeout_seconds < 1 or expected_exit < 0 or not os.access(executable, os.X_OK):
        print("invalid forked Kore test arguments", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="vectis-kore-test.", dir=os.getcwd()) as run_dir:
        read_fd, write_fd = os.pipe()
        supervisor = os.fork()
        if supervisor == 0:
            os.close(read_fd)
            try:
                supervise(write_fd, executable, timeout_seconds, run_dir)
            except BaseException as error:
                print("forked Kore supervisor failed: {}".format(error), file=sys.stderr)
                os._exit(125)
        os.close(write_fd)
        result = 125
        try:
            ready, _, _ = select.select([read_fd], [], [], timeout_seconds + 5)
            if ready:
                status = os.read(read_fd, 32).decode("ascii").strip()
                try:
                    actual_exit = int(status)
                    if actual_exit == expected_exit:
                        result = 0
                    else:
                        print("forked Kore test exited {}, expected {}".format(actual_exit, expected_exit), file=sys.stderr)
                        result = 1
                except ValueError:
                    pass
            if result == 125:
                print("forked Kore test did not report an exit status", file=sys.stderr)
        finally:
            os.close(read_fd)
            pid_file = os.path.join(run_dir, "kore.pid")
            if os.path.isfile(pid_file):
                try:
                    with open(pid_file, encoding="ascii") as stream:
                        kore_group = int(stream.read().strip())
                    if kore_group <= 0 or kore_group == supervisor or not stop_group(kore_group, supervisor):
                        result = 125
                except (OSError, ValueError) as error:
                    print("invalid Kore pid file: {}".format(error), file=sys.stderr)
                    result = 125
            elif expected_exit != 0:
                print("expected failure did not leave a Kore pid file", file=sys.stderr)
                result = 125
            if not stop_group(supervisor, supervisor):
                result = 125
                os.kill(supervisor, signal.SIGKILL)
            os.waitpid(supervisor, 0)
            for name in os.listdir(run_dir):
                if name.startswith(("asan.", "ubsan.")):
                    print("forked Kore worker sanitizer report: {}".format(name), file=sys.stderr)
                    with open(os.path.join(run_dir, name), encoding="utf-8", errors="replace") as stream:
                        print(stream.read(), file=sys.stderr)
                    result = 1
        return result


if __name__ == "__main__":
    sys.exit(main())
