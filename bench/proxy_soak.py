#!/usr/bin/env python3
"""Opt-in direct versus proxied SSE/WebSocket churn and shutdown soak."""

import argparse
from collections import deque
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import time

import proxy as common


ROOT = Path(__file__).resolve().parent.parent
MAX_LATENCY_SAMPLES = 10000


def wait_for_exit(process, owned_pids, start):
    process.wait(timeout=5)
    deadline = start + 5
    while time.monotonic() < deadline:
        if not any(Path(f"/proc/{pid}").exists() for pid in owned_pids):
            return round(time.monotonic() - start, 3)
        time.sleep(0.02)
    raise TimeoutError("Vectis workers remained after app shutdown")


def soak(host, port, secure, context, seconds, concurrency, events, echoes,
         process=None):
    stop = threading.Event()
    lock = threading.Lock()
    errors = []
    active = 0
    completed = {"sse": 0, "ws": 0}
    delays = {"sse": deque(maxlen=MAX_LATENCY_SAMPLES),
              "ws": deque(maxlen=MAX_LATENCY_SAMPLES)}

    def worker(kind):
        nonlocal active
        while not stop.is_set():
            ready = False

            def mark_ready():
                nonlocal active, ready
                with lock:
                    active += 1
                    ready = True

            try:
                if kind == "sse":
                    samples = common.sse_trial(host, port, secure, context,
                                               events, on_ready=mark_ready)
                else:
                    samples = common.ws_trial(host, port, secure, context, echoes,
                                              interval_seconds=0.01,
                                              on_ready=mark_ready)
                with lock:
                    completed[kind] += 1
                    delays[kind].extend(samples)
            except Exception as exc:
                if not stop.is_set():
                    with lock:
                        errors.append(f"{kind}: {exc}")
                    stop.set()
            finally:
                if ready:
                    with lock:
                        active -= 1

    threads = [threading.Thread(target=worker,
                                args=("sse" if index % 2 == 0 else "ws",),
                                daemon=True)
               for index in range(concurrency)]
    with common.ResourceSampler(process.pid if process else None) as sampler:
        for thread in threads:
            thread.start()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not stop.is_set():
            stop.wait(min(0.05, deadline - time.monotonic()))
        owned_pids = common.pids_for_group(process.pid) if process else None
        with lock:
            active_at_stop = active
            stop.set()
            shutdown_start = time.monotonic()
            if process is not None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    errors.append("Vectis exited before app shutdown")
        if process is not None and not errors and active_at_stop < 1:
            errors.append("no active stream at the shutdown boundary")
        shutdown_seconds = None
        if process is not None:
            try:
                shutdown_seconds = wait_for_exit(process, owned_pids,
                                                 shutdown_start)
            except Exception as exc:
                errors.append(str(exc))
        join_deadline = time.monotonic() + 8
        for thread in threads:
            thread.join(timeout=max(0, join_deadline - time.monotonic()))
        if any(thread.is_alive() for thread in threads):
            errors.append("client stream did not stop within eight seconds")
    if errors:
        raise RuntimeError("; ".join(errors))
    if not completed["sse"] or not completed["ws"]:
        raise RuntimeError("soak ended without completed SSE and WebSocket sessions")
    cpu_seconds = (
        (sampler.cpu_last - sampler.cpu_start) / os.sysconf("SC_CLK_TCK")
        if sampler.cpu_start is not None else None
    )
    if cpu_seconds is not None and cpu_seconds < 0:
        raise RuntimeError("sampled process CPU time decreased after shutdown")
    return {
        "completed_sessions": completed,
        "active_streams_at_stop": active_at_stop,
        "sse_event_delay": common.distribution(delays["sse"]),
        "ws_echo": common.distribution(delays["ws"]),
        "sample_limit_per_protocol": MAX_LATENCY_SAMPLES,
        "peak_worker_rss_bytes": sampler.peak_worker_rss_bytes or None,
        "peak_worker_fds": sampler.peak_worker_fds or None,
        "peak_process_group_fds": sampler.peak_fd_count or None,
        "process_group_cpu_seconds": round(cpu_seconds, 3)
        if cpu_seconds is not None else None,
        "app_shutdown_seconds": shutdown_seconds,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectis", type=Path, default=ROOT / "build/debug/vectis")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if args.smoke:
        args.seconds, args.concurrency = 2, 2
    if args.seconds < 2 or args.concurrency < 2 or args.concurrency > 16:
        parser.error("seconds must be at least two and concurrency must be 2 through 16")
    if args.concurrency % 2:
        parser.error("concurrency must be even to split SSE and WebSocket streams")
    if sys.platform != "linux":
        parser.error("soak resource and worker-exit checks require Linux /proc")
    binary = args.vectis.resolve()
    if not binary.is_file():
        parser.error(f"Vectis executable not found: {binary}")
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="proxy-soak-", dir=ROOT / "build") as temp:
        directory = Path(temp)
        proxy_port = common.free_port()
        origin = common.OriginServer(("127.0.0.1", 0), common.OriginHandler)
        origin_port = origin.server_port
        while proxy_port == origin_port:
            proxy_port = common.free_port()
        events = 20 if args.smoke else 100
        echoes = 20 if args.smoke else 100
        origin.chunks, origin.events, origin.slow_upload = 4, events, True
        context = None
        ca_pem = None
        bundle = None
        if args.tls:
            cert, key = directory / "origin.crt", directory / "origin.key"
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout", str(key), "-out", str(cert),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ca_pem = cert.read_text()
            bundle = directory / "server.pem"
            bundle.write_text(ca_pem + key.read_text())
            server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server_context.load_cert_chain(cert, key)
            origin.socket = server_context.wrap_socket(origin.socket,
                                                       server_side=True)
            context = ssl.create_default_context(cadata=ca_pem)
        origin_thread = threading.Thread(target=origin.serve_forever,
                                         daemon=True)
        origin_thread.start()
        process = None
        try:
            process = common.launch_proxy(binary, proxy_port, origin_port,
                                          directory, ca_pem, bundle)
            host = "localhost" if args.tls else "127.0.0.1"
            direct = soak(host, origin_port, args.tls, context, args.seconds,
                          args.concurrency, events, echoes)
            proxied = soak(host, proxy_port, args.tls, context, args.seconds,
                           args.concurrency, events, echoes, process)
            added = {}
            for name, metric in (("sse", "sse_event_delay"),
                                 ("ws", "ws_echo")):
                added[name] = {
                    percentile: round(proxied[metric][percentile]
                                      - direct[metric][percentile], 3)
                    for percentile in ("p50_ms", "p95_ms", "p99_ms")
                }
            print(json.dumps({
                "note": "Exploratory soak; set thresholds only on a dedicated runner",
                "parameters": {"seconds_per_path": args.seconds,
                               "concurrency": args.concurrency,
                               "upstream_tls": args.tls},
                "direct": direct, "proxied": proxied,
                "added_latency_ms": added,
            }, indent=2, sort_keys=True))
        except Exception:
            log_path = directory / "proxy.log"
            if log_path.is_file():
                failure = ROOT / "build/proxy-soak-failure.log"
                shutil.copyfile(log_path, failure)
                print(f"Vectis log saved to {failure}", file=sys.stderr)
            raise
        finally:
            if process is not None:
                common.stop_proxy(process)
            origin.shutdown()
            origin.server_close()
            origin_thread.join(timeout=5)


if __name__ == "__main__":
    main()
