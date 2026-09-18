"""Bound encrypted-Pouch recovery through real HTTPS readiness, not app:start alone."""

import argparse
import base64
import hashlib
import http.client
import json
import os
import pathlib
import signal
import socket
import ssl
import subprocess
import tempfile
import time


def stop(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    finally:
        # A supervisor may have exited before its workers; clean the whole group.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def cpu_seconds(pid):
    """Include active descendants and already reaped children, in scheduler ticks."""
    stat = pathlib.Path(f"/proc/{pid}/stat")
    try:
        fields = stat.read_text().rsplit(")", 1)[1].split()
        ticks = sum(int(fields[i]) for i in (11, 12, 13, 14))
        children = pathlib.Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
    except FileNotFoundError:
        return 0
    return ticks / os.sysconf("SC_CLK_TCK") + sum(cpu_seconds(int(child)) for child in children)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--updates", type=int, default=20000)
    parser.add_argument("--ready-seconds", type=float, default=5)
    parser.add_argument("--cpu-seconds", type=float, default=2)
    args = parser.parse_args()
    assert args.updates > 0 and args.ready_seconds > 0 and args.cpu_seconds > 0
    assert pathlib.Path("/proc/self/stat").exists(), "Linux /proc is required for the CPU gate"
    binary = args.binary.resolve()
    script = pathlib.Path(__file__).resolve().parent / "lua/metrics_startup.lua"
    key = "snapshot.v1." + hashlib.sha256(b"vectis\0metrics-startup-gate").hexdigest()
    env = dict(os.environ, VECTIS_POUCH_CRYPTO_KEY="lc-pouch-key-v1:" +
               base64.urlsafe_b64encode(bytes(range(32))).decode().rstrip("="))
    context = ssl._create_unverified_context()
    with tempfile.TemporaryDirectory(prefix="metrics-startup-", dir=binary.parent) as work:
        for label, count, crash, shared in (
            ("small", 1, False, False), ("churn", args.updates, False, False),
            ("crash-shared", args.updates, True, True), ("crash-exclusive", 1, True, False)
        ):
            root = pathlib.Path(work) / label
            root.mkdir()
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem"),
                            "-subj", "/CN=localhost", "-days", "1"],
                           check=True, capture_output=True, timeout=10)
            (root / "tls.pem").write_bytes((root / "cert.pem").read_bytes() +
                                           (root / "key.pem").read_bytes())
            with (root / "seed.log").open("w+") as log:
                process = subprocess.Popen([str(binary), str(script), "seed", str(root), key,
                                            str(count), "crash" if crash else "clean",
                                            "shared" if shared else "exclusive"],
                                           env=env, cwd=root, stdout=log, stderr=log,
                                           start_new_session=True)
                try:
                    deadline = time.monotonic() + 300
                    while process.poll() is None:
                        if crash and "SEEDED" in (root / "seed.log").read_text():
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
                            break
                        assert time.monotonic() < deadline, f"{label}: fixture exceeded 300s"
                        time.sleep(.05)
                    assert process.returncode == (-signal.SIGKILL if crash else 0), (root / "seed.log").read_text()
                finally:
                    stop(process)
            size = sum(p.stat().st_size for p in (root / "state").rglob("*") if p.is_file())
            for restart in range(2):
                with socket.socket() as sock:
                    sock.bind(("127.0.0.1", 0))
                    port = sock.getsockname()[1]
                with (root / f"start-{restart}.log").open("w") as log:
                    start = time.monotonic()
                    process = subprocess.Popen([str(binary), str(script), "serve", str(root), key,
                                                str(port), "unused", "shared" if shared else "exclusive"],
                                               env=env, cwd=root, stdout=log, stderr=log,
                                               start_new_session=True)
                    try:
                        if crash and not shared:
                            process.wait(timeout=args.ready_seconds)
                            assert process.returncode != 0
                            assert "pouch root has an unexpired exclusive writer" in pathlib.Path(log.name).read_text()
                            print("metrics startup: crash-exclusive rejected promptly with lockdc lease diagnostic",
                                  flush=True)
                            break
                        while True:
                            assert process.poll() is None, f"{label}: exited; {pathlib.Path(log.name).read_text()}"
                            elapsed = time.monotonic() - start
                            assert elapsed < args.ready_seconds, (
                                f"{label}: HTTPS not ready after {elapsed:.3f}s; "
                                f"updates={count} bytes={size}; {pathlib.Path(log.name).read_text()}")
                            connection = http.client.HTTPSConnection("localhost", port,
                                                                      timeout=.2, context=context)
                            try:
                                connection.request("GET", "/.metrics.json")
                                response = connection.getresponse()
                                body = response.read()
                                if (response.status == 503 and
                                        body == b"vectis app is starting\n"):
                                    # The listener can accept just before the
                                    # supervisor publishes its ready state.
                                    # This is a transient readiness signal, not
                                    # a successfully served metrics response.
                                    time.sleep(.025)
                                    continue
                                assert response.status == 200, body
                                snapshot = json.loads(body)
                                assert snapshot["http"]["requests_total"] >= count, snapshot
                                break
                            except (OSError, http.client.HTTPException):
                                time.sleep(.025)
                            finally:
                                connection.close()
                        elapsed = time.monotonic() - start
                        cpu = cpu_seconds(process.pid)
                        assert elapsed < args.ready_seconds, f"{label}: readiness took {elapsed:.3f}s"
                        assert cpu < args.cpu_seconds, f"{label}: startup consumed {cpu:.3f} CPU seconds"
                        print(f"metrics startup: {label} restart={restart} updates={count} "
                              f"bytes={size} https_ready={elapsed:.3f}s cpu={cpu:.3f}s", flush=True)
                    finally:
                        stop(process)


if __name__ == "__main__":
    main()
