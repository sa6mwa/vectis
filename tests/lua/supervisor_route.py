#!/usr/bin/env python3
"""Real worker commit -> FD wake -> owner Lua handler, without sleep polling."""
import argparse
import http.client
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import socket
import subprocess
import time


def scenario(command, root, concurrent=True):
    root.mkdir(parents=True)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    environment = dict(os.environ, VECTIS_WORKFLOW_ROUTE_ROOT=str(root),
                       VECTIS_WORKFLOW_ROUTE_PORT=str(port), TMPDIR=str(root),
                       XDG_STATE_HOME=str(root / "state"), XDG_CONFIG_HOME=str(root / "config"),
                       VECTIS_WORKFLOW_ROUTE_PROBE_MS="10" if concurrent else "1000000")
    with (root / "stderr.log").open("wb") as errors:
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                   stderr=errors, start_new_session=True)
        pending = b""
        lines = []
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)

        def until(predicate):
            nonlocal pending
            deadline = time.monotonic() + 5
            while not predicate():
                remaining = deadline - time.monotonic()
                assert remaining > 0 and selector.select(remaining), ("missing event", lines)
                data = os.read(process.stdout.fileno(), 4096)
                assert data, ("premature exit", process.poll(), (root / "stderr.log").read_text())
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    lines.append(line.decode())
        try:
            until(lambda: "ready" in lines and "sus drained" in lines)
            # Independent connections allow Kore's two workers to accept and
            # produce. Application response asserts no handler ran there.
            connections = []
            total = 8
            for i in range(total):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                connections.append(connection)
                identity = f"{'two' if i % 2 else 'one'}-{i}"
                connection.request("POST", "/effect", identity,
                                   {"Content-Type": "text/plain", "Connection": "close"})
                if not concurrent:
                    response = connection.getresponse()
                    data = json.loads(response.read())
                    assert response.status == 200 and data == {"receipts": 1, "effects_in_producer": 0}, data
                    connection.close()
                    until(lambda: f"effect {identity}" in lines)
            if not concurrent:
                connections = []
            for connection in connections:
                response = connection.getresponse()
                body = response.read()
                assert response.status == 200, (response.status, body, lines, (root / "stderr.log").read_text())
                data = json.loads(body)
                assert response.status == 200 and data == {"receipts": 1, "effects_in_producer": 0}, data
                connection.close()
            expected = {f"effect {'two' if i % 2 else 'one'}-{i}" for i in range(total)}
            until(lambda: expected.issubset(lines))
            assert len([line for line in lines if line.startswith("effect ")]) == total, lines
            until(lambda: "sus drained" in lines)
            # A duplicate commit cannot produce a second effect.
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            connection.request("POST", "/effect", "one-0", {"Connection": "close"})
            response = connection.getresponse(); assert response.status == 200
            response.read(); connection.close()
            process.send_signal(signal.SIGTERM)
            until(lambda: f"stopped {total}" in lines)
            assert process.wait(timeout=5) == 0, (root / "stderr.log").read_text()
        finally:
            selector.close()
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vectis", required=True)
    parser.add_argument("--script", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    work = Path(args.work).resolve()
    assert(work.is_relative_to(Path(__file__).resolve().parents[2] / "build"))
    shutil.rmtree(work, ignore_errors=True)
    scenario([args.vectis, args.script], work / "normal-direct", concurrent=False)
    scenario([args.vectis, args.script], work / "normal")
    if __import__("sys").platform == "darwin":
        print("supervisor route normal owner dispatch ok (Linux packing excluded)")
        return
    packed = work / "packed"
    result = subprocess.run([args.vectis, "-a", "pack", "--script", args.script,
                             "--output", str(packed)], capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    scenario([str(packed)], work / "packed-direct", concurrent=False)
    scenario([str(packed)], work / "packed-root")
    print("supervisor route normal/packed owner dispatch ok")


if __name__ == "__main__":
    main()
