#!/usr/bin/env python3
"""Opt-in direct HTTP/2 versus HTTP/1.1-to-HTTP/2 Vectis measurements."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import proxy as common


ROOT = Path(__file__).resolve().parent.parent


def read_stats(helper, url, ca, expected_protocol, args, resource_pid):
    command = [
        str(helper), "client", "--base", url, "--ca", str(ca),
        "--expect-proto", str(expected_protocol), "--chunks", str(args.chunks),
        "--events", str(args.events), "--warmup", str(args.warmup),
        "--repetitions", str(args.repetitions), "--concurrency",
        ",".join(map(str, args.concurrency)),
    ]
    with common.ResourceSampler(resource_pid) as sampler:
        result = subprocess.run(command, check=False, capture_output=True,
                                text=True, timeout=180)
    if result.returncode != 0:
        raise RuntimeError(f"H2 client failed: {result.stderr.strip()}")
    raw = json.loads(result.stdout)
    if raw["origin_protocol"] != "HTTP/2.0":
        raise RuntimeError("origin did not receive HTTP/2")
    profiles = {}
    for name, samples in raw["http"].items():
        if len(samples) != args.repetitions:
            raise RuntimeError(f"{name}: wrong sample count")
        mean_completion = sum(item["completion_ms"] for item in samples) / len(samples)
        payload_bytes = (
            2 if name == "small" else args.chunks * 16384
        )
        profiles[name] = {
            "first_byte": common.distribution([item["first_ms"] for item in samples]),
            "completion": common.distribution([item["completion_ms"] for item in samples]),
            "response_bytes_per_trial": samples[0]["bytes"],
            "payload_bytes_per_trial": payload_bytes,
            "mean_mib_per_second": round(
                payload_bytes / 1048576 / (mean_completion / 1000), 3
            ),
        }
    for name, delays in raw["sse"].items():
        count = int(name.removeprefix("sse_"))
        if len(delays) != args.repetitions * count * args.events:
            raise RuntimeError(f"{name}: wrong SSE event count")
        profiles[name] = {"event_delay": common.distribution(delays)}
    profiles["resource"] = {
        "peak_process_group_rss_bytes": sampler.peak_process_group_rss_bytes or None,
        "peak_process_group_fds": sampler.peak_fd_count or None,
        "peak_worker_rss_bytes": sampler.peak_worker_rss_bytes or None,
        "peak_worker_fds": sampler.peak_worker_fds or None,
        "process_group_cpu_seconds": round(
            (sampler.cpu_last - sampler.cpu_start) / os.sysconf("SC_CLK_TCK"), 3
        ) if sampler.cpu_start is not None else None,
    }
    return raw["downstream_protocol"], profiles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectis", type=Path, default=ROOT / "build/debug/vectis")
    parser.add_argument("--helper", type=Path, default=ROOT / "build/proxy_h2_helper")
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--chunks", type=int, default=256)
    parser.add_argument("--events", type=int, default=5)
    parser.add_argument("--concurrency", type=int, nargs="+", default=[1, 8, 16])
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if args.smoke:
        args.warmup, args.repetitions, args.chunks = 0, 1, 4
        args.events, args.concurrency = 2, [1]
    if args.warmup < 0 or args.repetitions < 1 or args.chunks < 2 or args.events < 1:
        parser.error("warmup, repetitions, chunks, or events out of range")
    if (not args.concurrency
            or any(value < 1 or value > 16 for value in args.concurrency)
            or len(set(args.concurrency)) != len(args.concurrency)):
        parser.error("concurrency must contain distinct values from 1 through 16")
    binary, helper = args.vectis.resolve(), args.helper.resolve()
    if not binary.is_file() or not helper.is_file():
        parser.error("build Vectis and the Go helper with make bench-proxy-h2")
    with tempfile.TemporaryDirectory(prefix="proxy-h2-bench-", dir=ROOT / "build") as temp:
        directory = Path(temp)
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
        origin_port = common.free_port()
        proxy_port = common.free_port()
        while proxy_port == origin_port:
            proxy_port = common.free_port()
        origin_log = directory / "origin.log"
        with origin_log.open("wb") as output:
            origin = subprocess.Popen([
                str(helper), "origin", "--listen", f"127.0.0.1:{origin_port}",
                "--cert", str(cert), "--key", str(key),
                "--chunks", str(args.chunks), "--events", str(args.events),
            ], cwd=directory, stdout=output, stderr=subprocess.STDOUT,
                start_new_session=True)
        proxy = None
        try:
            common.wait_ready(origin_port, origin, origin_log)
            proxy = common.launch_proxy(binary, proxy_port, origin_port,
                                        directory, ca_pem, bundle)
            direct_protocol, direct = read_stats(
                helper, f"https://localhost:{origin_port}", cert, 2, args, origin.pid
            )
            proxy_protocol, proxied = read_stats(
                helper, f"https://localhost:{proxy_port}", cert, 1, args, proxy.pid
            )
            if direct.keys() != proxied.keys():
                raise RuntimeError("direct and proxied profiles differ")
            for name in direct:
                if name != "resource" and not name.startswith("sse_") and (
                    direct[name]["response_bytes_per_trial"]
                    != proxied[name]["response_bytes_per_trial"]
                ):
                    raise RuntimeError(f"{name}: direct and proxied byte counts differ")
            added = {}
            for name, measured in direct.items():
                if name == "resource":
                    continue
                metrics = ("event_delay",) if name.startswith("sse_") else (
                    "first_byte", "completion"
                )
                added[name] = {
                    metric: {
                        percentile: round(
                            proxied[name][metric][percentile]
                            - measured[metric][percentile], 3
                        )
                        for percentile in ("p50_ms", "p95_ms", "p99_ms")
                    }
                    for metric in metrics
                }
            print(json.dumps({
                "note": "Exploratory local measurement; use an isolated runner for thresholds",
                "parameters": {
                    "warmup": args.warmup, "repetitions": args.repetitions,
                    "chunk_bytes": 16384, "chunks": args.chunks,
                    "events": args.events, "concurrency": args.concurrency,
                    "direct_protocol": direct_protocol,
                    "proxy_downstream_protocol": proxy_protocol,
                    "origin_protocol": "HTTP/2.0",
                },
                "direct": direct, "proxied": proxied,
                "added_latency_ms": added,
            }, indent=2, sort_keys=True))
        except Exception:
            for path in (directory / "proxy.log", origin_log):
                if path.is_file():
                    failure = ROOT / "build" / f"proxy-h2-{path.name}"
                    shutil.copyfile(path, failure)
                    print(f"H2 log saved to {failure}", file=sys.stderr)
            raise
        finally:
            if proxy is not None:
                common.stop_proxy(proxy)
            common.stop_proxy(origin)


if __name__ == "__main__":
    main()
