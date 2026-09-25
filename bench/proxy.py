#!/usr/bin/env python3
"""Opt-in, local direct-versus-Vectis proxy latency and resource measurements.

This is a measurement tool, not a performance gate. Payloads are generated and
consumed in chunks. Run it on an isolated host for comparable results.
"""

import argparse
import asyncio
import base64
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


CHUNK = 16384
PAYLOAD = b"v" * CHUNK
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOT = Path(__file__).resolve().parent.parent


def read_exact(sock, count):
    parts = bytearray()
    while len(parts) < count:
        data = sock.recv(count - len(parts))
        if not data:
            raise EOFError("socket closed before frame completed")
        parts.extend(data)
    return bytes(parts)


class OriginHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path == "/small":
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
        elif self.path == "/download":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for _ in range(self.server.chunks):
                self.wfile.write(b"4000\r\n" + PAYLOAD + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/sse":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for _ in range(self.server.events):
                event = b"data: " + str(time.monotonic_ns()).encode() + b"\n\n"
                self.wfile.write(f"{len(event):x}\r\n".encode() + event + b"\r\n")
                self.wfile.flush()
                time.sleep(0.01)
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/ws":
            self.websocket()
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path not in ("/upload", "/duplex"):
            self.send_error(404)
            return
        remaining = int(self.headers["Content-Length"])
        total = remaining
        if self.path == "/duplex":
            first = self.rfile.read(min(CHUNK, remaining))
            if not first:
                raise EOFError("duplex upload truncated")
            remaining -= len(first)
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(f"{len(first):x}\r\n".encode() + first + b"\r\n")
            self.wfile.flush()
        while remaining:
            data = self.rfile.read(min(CHUNK, remaining))
            if not data:
                raise EOFError("upload truncated")
            remaining -= len(data)
            if self.path == "/duplex":
                self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
                self.wfile.flush()
            if self.server.slow_upload:
                time.sleep(0.0005)
        if self.path == "/duplex":
            self.wfile.write(b"0\r\n\r\n")
            return
        body = str(total).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "").encode()
        if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            self.send_error(400)
            return
        accept = base64.b64encode(hashlib.sha1(key + WS_GUID).digest())
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        while True:
            try:
                frame = read_exact(self.connection, 2)
            except EOFError:
                return
            opcode = frame[0] & 15
            length = frame[1] & 127
            if length == 126:
                length = int.from_bytes(read_exact(self.connection, 2), "big")
            if not frame[1] & 128 or length > CHUNK:
                raise ValueError("expected a bounded masked frame")
            mask = read_exact(self.connection, 4)
            body = read_exact(self.connection, length)
            body = bytes(b ^ mask[i % 4] for i, b in enumerate(body))
            if opcode == 8:
                return
            if opcode != 2:
                raise ValueError("expected binary frame")
            header = (
                bytes((0x82, length))
                if length < 126 else b"\x82\x7e" + length.to_bytes(2, "big")
            )
            self.connection.sendall(header + body)


class OriginServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def percentile(values, fraction):
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return round(ordered[index], 3)


def distribution(values):
    return {
        "samples": len(values),
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "mean_ms": round(statistics.mean(values), 3),
    }


def transfer_rates(request_bytes, response_bytes, completion_ms):
    seconds = completion_ms / 1000
    return {
        "request_bytes_per_trial": request_bytes,
        "response_bytes_per_trial": response_bytes,
        "mean_upload_mib_per_second": round(
            request_bytes / 1048576 / seconds, 3
        ) if request_bytes else None,
        "mean_download_mib_per_second": round(
            response_bytes / 1048576 / seconds, 3
        ) if response_bytes else None,
    }


def connection(host, port, secure, context):
    if secure:
        return http.client.HTTPSConnection(host, port, timeout=15, context=context)
    return http.client.HTTPConnection(host, port, timeout=15)


def http_trial(host, port, secure, context, path, chunks=0, slow_reader=False):
    conn = connection(host, port, secure, context)
    start = time.monotonic_ns()
    try:
        if path == "/upload":
            conn.putrequest("POST", path)
            conn.putheader("Content-Length", str(chunks * CHUNK))
            conn.endheaders()
            for _ in range(chunks):
                conn.send(PAYLOAD)
        else:
            conn.request("GET", path)
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"{path}: unexpected status {response.status}")
        first = None
        total = 0
        short_body = bytearray()
        while True:
            data = response.read1(CHUNK)
            if not data:
                break
            if first is None:
                first = time.monotonic_ns()
            total += len(data)
            if path == "/download":
                if data.count(b"v") != len(data):
                    raise RuntimeError("download payload mismatch")
            else:
                short_body.extend(data)
                if len(short_body) > 32:
                    raise RuntimeError("short response exceeded 32 bytes")
            if slow_reader:
                time.sleep(0.0005)
        end = time.monotonic_ns()
        expected = chunks * CHUNK if path == "/download" else 2
        if path == "/upload":
            expected = len(str(chunks * CHUNK))
        if total != expected:
            raise RuntimeError(f"{path}: received {total} bytes, expected {expected}")
        if path == "/small" and short_body != b"ok":
            raise RuntimeError("small response payload mismatch")
        if path == "/upload" and short_body != str(chunks * CHUNK).encode():
            raise RuntimeError("upload count mismatch")
        return ((first - start) / 1e6, (end - start) / 1e6, total)
    finally:
        conn.close()


def sse_trial(host, port, secure, context, events, on_ready=None):
    conn = connection(host, port, secure, context)
    try:
        conn.request("GET", "/sse")
        response = conn.getresponse()
        if response.status != 200 or response.getheader("Content-Type") != "text/event-stream":
            raise RuntimeError("SSE status/content type mismatch")
        if on_ready is not None:
            on_ready()
        delays = []
        pending = b""
        while len(delays) < events:
            data = response.read1(CHUNK)
            if not data:
                raise EOFError("SSE stream ended early")
            pending += data
            while b"\n\n" in pending and len(delays) < events:
                event, pending = pending.split(b"\n\n", 1)
                if not event.startswith(b"data: "):
                    raise ValueError("invalid SSE event")
                delays.append((time.monotonic_ns() - int(event[6:])) / 1e6)
        if response.read1(1):
            raise RuntimeError("SSE origin sent an unexpected extra event")
        return delays
    finally:
        conn.close()


async def duplex_async(host, port, secure, context, chunks):
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(
            host, port, ssl=context if secure else None,
            server_hostname=host if secure else None,
        ), timeout=15,
    )
    first_response = asyncio.Event()
    upload_done = None
    start = time.monotonic_ns()

    async def send_upload():
        nonlocal upload_done
        header = (
            f"POST /duplex HTTP/1.1\r\nHost: {host}:{port}\r\n"
            f"Content-Length: {chunks * CHUNK}\r\nConnection: close\r\n\r\n"
        ).encode()
        writer.write(header + PAYLOAD)
        await writer.drain()
        await asyncio.wait_for(first_response.wait(), timeout=10)
        for _ in range(chunks - 1):
            writer.write(PAYLOAD)
            await writer.drain()
            await asyncio.sleep(0.001)
        upload_done = time.monotonic_ns()

    upload = asyncio.create_task(send_upload())
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=15)
        if (not head.startswith(b"HTTP/1.1 200 ") or
                b"transfer-encoding: chunked" not in head.lower()):
            raise RuntimeError(f"duplex: unexpected response headers {head!r}")
        first = None
        total = 0
        while True:
            size_line = await asyncio.wait_for(reader.readline(), timeout=15)
            length = int(size_line.split(b";", 1)[0], 16)
            if length == 0:
                await asyncio.wait_for(reader.readexactly(2), timeout=15)
                break
            data = await asyncio.wait_for(reader.readexactly(length), timeout=15)
            if await asyncio.wait_for(reader.readexactly(2), timeout=15) != b"\r\n":
                raise RuntimeError("duplex: invalid chunk terminator")
            if first is None:
                first = time.monotonic_ns()
                first_response.set()
            if data.count(b"v") != len(data):
                raise RuntimeError("duplex response payload mismatch")
            total += len(data)
        end = time.monotonic_ns()
        await asyncio.wait_for(upload, timeout=15)
        if total != chunks * CHUNK or upload_done is None:
            raise RuntimeError("duplex transfer length mismatch")
        if first >= upload_done:
            raise RuntimeError("duplex response began only after upload EOF")
        return ((first - start) / 1e6, (end - start) / 1e6, total)
    finally:
        upload.cancel()
        writer.close()
        try:
            await asyncio.wait_for(writer.wait_closed(), timeout=5)
        except (OSError, asyncio.TimeoutError):
            pass


def duplex_trial(host, port, secure, context, chunks):
    return asyncio.run(duplex_async(host, port, secure, context, chunks))


def ws_trial(host, port, secure, context, echoes, interval_seconds=0,
             on_ready=None):
    sock = socket.create_connection((host, port), 15)
    if secure:
        sock = context.wrap_socket(sock, server_hostname=host)
    sock.settimeout(15)
    try:
        key = base64.b64encode(os.urandom(16))
        request = (
            b"GET /ws HTTP/1.1\r\nHost: " + host.encode() + b":" + str(port).encode()
            + b"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            + b"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + b"\r\n\r\n"
        )
        sock.sendall(request)
        headers = bytearray()
        while not headers.endswith(b"\r\n\r\n"):
            headers.extend(read_exact(sock, 1))
            if len(headers) > 8192:
                raise ValueError("WebSocket response headers too large")
        expected = base64.b64encode(hashlib.sha1(key + WS_GUID).digest())
        if (not headers.startswith(b"HTTP/1.1 101 ") or
                b"Sec-WebSocket-Accept: " + expected not in headers):
            raise RuntimeError(f"WebSocket handshake rejected: {headers!r}")
        if on_ready is not None:
            on_ready()
        delays = []
        for _ in range(echoes):
            body = b"proxy-benchmark"
            mask = os.urandom(4)
            masked = bytes(b ^ mask[i % 4] for i, b in enumerate(body))
            start = time.monotonic_ns()
            sock.sendall(b"\x82" + bytes((0x80 | len(body),)) + mask + masked)
            frame = read_exact(sock, 2)
            if frame != bytes((0x82, len(body))) or read_exact(sock, len(body)) != body:
                raise RuntimeError("WebSocket echo mismatch")
            delays.append((time.monotonic_ns() - start) / 1e6)
            if interval_seconds:
                time.sleep(interval_seconds)
        sock.sendall(b"\x88\x80" + os.urandom(4))
        sock.settimeout(5)
        if sock.recv(1):
            raise RuntimeError("WebSocket origin sent data after close")
        return delays
    finally:
        sock.close()


def pids_for_group(master):
    result = {master}
    queue = [master]
    while queue:
        pid = queue.pop()
        try:
            children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        except OSError:
            continue
        for child in children:
            child = int(child)
            if child not in result:
                result.add(child)
                queue.append(child)
    return result


class ResourceSampler:
    def __init__(self, pid):
        self.pid = pid
        self.stop = threading.Event()
        self.peak_worker_rss_bytes = 0
        self.peak_process_group_rss_bytes = 0
        self.peak_fd_count = 0
        self.peak_worker_fds = 0
        self.cpu_start = None
        self.cpu_last = None
        self.cpu_ticks = 0
        self.cpu_by_process = {}
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while not self.stop.is_set():
            pids = pids_for_group(self.pid)
            fd_count = 0
            group_rss = 0
            for pid in pids:
                try:
                    status = Path(f"/proc/{pid}/status").read_text()
                    rss = next(
                        int(line.split()[1]) * 1024 for line in status.splitlines()
                        if line.startswith("VmRSS:")
                    )
                    group_rss += rss
                    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
                    process_key = (pid, int(fields[19]))
                    cpu_ticks = int(fields[11]) + int(fields[12])
                    previous = self.cpu_by_process.get(process_key)
                    if previous is not None:
                        self.cpu_ticks += max(0, cpu_ticks - previous)
                    self.cpu_by_process[process_key] = cpu_ticks
                    if pid != self.pid:
                        self.peak_worker_rss_bytes = max(self.peak_worker_rss_bytes, rss)
                    fds = len(os.listdir(f"/proc/{pid}/fd"))
                    fd_count += fds
                    if pid != self.pid:
                        self.peak_worker_fds = max(self.peak_worker_fds, fds)
                except (OSError, StopIteration):
                    continue
            self.peak_fd_count = max(self.peak_fd_count, fd_count)
            self.peak_process_group_rss_bytes = max(
                self.peak_process_group_rss_bytes, group_rss
            )
            if self.cpu_start is None:
                self.cpu_start = 0
            self.cpu_last = self.cpu_ticks
            self.stop.wait(0.02)

    def __enter__(self):
        if sys.platform == "linux" and self.pid is not None:
            self.thread.start()
        return self

    def __exit__(self, *_args):
        if self.thread.is_alive():
            self.stop.set()
            self.thread.join()


def concurrent_trials(count, trial):
    results = [None] * count
    errors = []
    barrier = threading.Barrier(count)

    def run(index):
        try:
            barrier.wait(timeout=10)
            results[index] = trial()
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=run, args=(i,)) for i in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=30)
    if any(thread.is_alive() for thread in threads):
        raise TimeoutError("concurrent profile did not finish")
    if errors:
        raise errors[0]
    return [item for result in results for item in result]


def wait_ready(port, process, log_path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"Vectis exited early ({process.returncode}): {log_path.read_text()}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), 0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"Vectis did not listen on {port}: {log_path.read_text()}")


def launch_proxy(binary, port, origin_port, directory, ca_pem=None, tls_bundle=None,
                 buffer_limit=None):
    ca_field = ""
    if ca_pem:
        ca_field = f", tls_ca_pem = {json.dumps(ca_pem)}"
    scheme = "https" if ca_pem else "http"
    buffer_field = f", buffer_limit_bytes={buffer_limit}" if buffer_limit else ""
    lua = directory / "proxy.lua"
    routes = ("proto", "small", "download", "upload", "duplex", "sse")
    server_tls = (
        f'{{mode="manual", cert_key_bundle_path={json.dumps(str(tls_bundle))}, '
        f'domain="localhost"}}'
        if tls_bundle else '{mode="disabled"}'
    )
    lines = [
        'local vectis = require("vectis")',
        f'local server = assert(vectis.app.new({{bind="127.0.0.1", '
        f'port={port}, tls={server_tls}, worker_count=1}}))',
    ]
    for path in routes:
        lines.append(
            f'assert(server:proxy({{path="/{path}", '
            f'target="{scheme}://127.0.0.1:{origin_port}"{ca_field}{buffer_field}}}) == true)'
        )
    lines.append(
        f'assert(server:proxy({{path="/ws", '
        f'target="{scheme}://127.0.0.1:{origin_port}"{ca_field}{buffer_field}}}) == true)'
    )
    lines.extend((
        'assert(server:start() == true)',
        'assert(server:wait() == true)',
        'server:close()',
    ))
    lua.write_text("\n".join(lines) + "\n")
    log_path = directory / "proxy.log"
    log = log_path.open("wb")
    try:
        process = subprocess.Popen(
            [str(binary), str(lua)], cwd=directory, stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True,
        )
    finally:
        log.close()
    try:
        wait_ready(port, process, log_path)
    except Exception:
        stop_proxy(process)
        raise
    return process


def stop_proxy(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)
    time.sleep(0.1)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def measure(host, port, secure, context, args, worker_pid=None):
    result = {}
    harness_cpu_start = time.process_time()
    with ResourceSampler(worker_pid) as sampler:
        for profile, path, chunks, slow in (
            ("small", "/small", 0, False),
            ("download", "/download", args.chunks, False),
            ("slow_reader", "/download", args.chunks, True),
            ("slow_upload", "/upload", args.chunks, False),
        ):
            if profile not in args.profiles:
                continue
            for _ in range(args.warmup):
                http_trial(host, port, secure, context, path, chunks, slow)
            trials = [
                http_trial(host, port, secure, context, path, chunks, slow)
                for _ in range(args.repetitions)
            ]
            mean_completion_ms = statistics.mean(item[1] for item in trials)
            result[profile] = {
                "first_byte": distribution([item[0] for item in trials]),
                "completion": distribution([item[1] for item in trials]),
                "mean_requests_per_second": round(
                    1000 / mean_completion_ms, 3
                ),
                **transfer_rates(chunks * CHUNK if path == "/upload" else 0,
                                 trials[0][2], mean_completion_ms),
            }
        if "duplex" in args.profiles:
            for _ in range(args.warmup):
                duplex_trial(host, port, secure, context, args.chunks)
            duplex = [
                duplex_trial(host, port, secure, context, args.chunks)
                for _ in range(args.repetitions)
            ]
            mean_completion_ms = statistics.mean(item[1] for item in duplex)
            result["duplex"] = {
                "first_byte": distribution([item[0] for item in duplex]),
                "completion": distribution([item[1] for item in duplex]),
                "mean_requests_per_second": round(
                    1000 / mean_completion_ms, 3
                ),
                **transfer_rates(chunks * CHUNK, duplex[0][2],
                                 mean_completion_ms),
            }
        for count in args.concurrency:
            for _ in range(args.warmup):
                if "sse" in args.profiles:
                    concurrent_trials(
                        count, lambda: sse_trial(host, port, secure, context, args.events)
                    )
                if "ws" in args.profiles:
                    concurrent_trials(
                        count, lambda: ws_trial(host, port, secure, context, args.echoes)
                    )
            if "sse" in args.profiles:
                sse = concurrent_trials(
                    count, lambda: sse_trial(host, port, secure, context, args.events)
                )
                result[f"sse_{count}"] = {"event_delay": distribution(sse)}
            if "ws" in args.profiles:
                ws = concurrent_trials(
                    count, lambda: ws_trial(host, port, secure, context, args.echoes)
                )
                result[f"ws_{count}"] = {"echo": distribution(ws)}
    result["resource"] = {
        "peak_worker_rss_bytes": sampler.peak_worker_rss_bytes or None,
        "peak_process_group_rss_bytes": sampler.peak_process_group_rss_bytes or None,
        "peak_worker_fds": sampler.peak_worker_fds or None,
        "peak_process_group_fds": sampler.peak_fd_count or None,
        "process_group_cpu_seconds": round(
            (sampler.cpu_last - sampler.cpu_start) / os.sysconf("SC_CLK_TCK"), 3
        ) if sampler.cpu_start is not None else None,
        "harness_cpu_seconds": round(time.process_time() - harness_cpu_start, 3),
    }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectis", type=Path, default=ROOT / "build/debug/vectis")
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--chunks", type=int, default=256, help="16 KiB chunks per transfer")
    parser.add_argument("--events", type=int, default=5)
    parser.add_argument("--echoes", type=int, default=5)
    parser.add_argument("--concurrency", type=int, nargs="+", default=[1, 8, 16])
    parser.add_argument("--profiles", nargs="+", default=[
        "small", "download", "slow_reader", "slow_upload", "duplex", "sse", "ws"
    ], choices=("small", "download", "slow_reader", "slow_upload",
                "duplex", "sse", "ws"),
                        help="run only the selected measurement profiles")
    parser.add_argument("--tls", action="store_true", help="use a local trusted HTTPS/WSS origin")
    parser.add_argument("--buffer-limit", type=int, default=16384,
                        help="proxy route buffer limit in bytes")
    parser.add_argument("--smoke", action="store_true", help="short harness verification")
    args = parser.parse_args()
    if args.smoke:
        args.repetitions, args.chunks, args.events, args.echoes, args.concurrency = 1, 4, 2, 2, [1]
        args.warmup = 0
    if args.warmup < 0:
        parser.error("warmup must be nonnegative")
    if not 8192 <= args.buffer_limit <= 1048576:
        parser.error("buffer limit must be between 8192 and 1048576 bytes")
    if min(args.repetitions, args.chunks, args.events, args.echoes, *args.concurrency) < 1:
        parser.error("repetitions, chunks, events, echoes, and concurrency must be positive")
    if args.chunks < 2 and "duplex" in args.profiles:
        parser.error("chunks must be at least 2 for the duplex streaming check")
    binary = args.vectis.resolve()
    if not binary.is_file():
        parser.error(f"Vectis executable not found: {binary}")
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="proxy-bench-", dir=ROOT / "build") as temp:
        directory = Path(temp)
        proxy_port = free_port()
        origin = OriginServer(("127.0.0.1", 0), OriginHandler)
        origin_port = origin.server_port
        while proxy_port == origin_port:
            proxy_port = free_port()
        origin.chunks, origin.events, origin.slow_upload = args.chunks, args.events, True
        client_context = None
        ca_pem = None
        tls_bundle = None
        if args.tls:
            cert, key = directory / "origin.crt", directory / "origin.key"
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout", str(key), "-out", str(cert),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ca_pem = cert.read_text()
            tls_bundle = directory / "server.pem"
            tls_bundle.write_text(ca_pem + key.read_text())
            server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server_context.load_cert_chain(cert, key)
            origin.socket = server_context.wrap_socket(origin.socket, server_side=True)
            client_context = ssl.create_default_context(cadata=ca_pem)
        thread = threading.Thread(target=origin.serve_forever, daemon=True)
        thread.start()
        process = None
        try:
            process = launch_proxy(binary, proxy_port, origin_port, directory,
                                   ca_pem, tls_bundle, args.buffer_limit)
            client_host = "localhost" if args.tls else "127.0.0.1"
            direct = measure(client_host, origin_port, args.tls, client_context, args)
            proxied = measure(client_host, proxy_port, args.tls, client_context, args, process.pid)
            added = {}
            for profile in direct:
                if profile == "resource":
                    continue
                keys = ("first_byte", "completion") if profile in (
                    "small", "download", "slow_reader", "slow_upload", "duplex"
                ) else (("event_delay",) if profile.startswith("sse_") else ("echo",))
                added[profile] = {
                    metric: {
                        percentile: round(
                            proxied[profile][metric][percentile]
                            - direct[profile][metric][percentile], 3
                        )
                        for percentile in ("p50_ms", "p95_ms", "p99_ms")
                    }
                    for metric in keys
                }
            print(json.dumps({
                "note": "Exploratory local measurement; use an isolated runner for thresholds",
                "parameters": {"warmup": args.warmup,
                               "repetitions": args.repetitions, "chunk_bytes": CHUNK,
                               "chunks": args.chunks, "events": args.events,
                               "echoes": args.echoes, "concurrency": args.concurrency,
                               "upstream_tls": args.tls,
                               "buffer_limit_bytes": args.buffer_limit,
                               "profiles": args.profiles},
                "direct": direct, "proxied": proxied,
                "added_latency_ms": added,
            }, indent=2, sort_keys=True))
        except Exception:
            log_path = directory / "proxy.log"
            if log_path.is_file():
                failure_log = ROOT / "build/proxy-bench-failure.log"
                shutil.copyfile(log_path, failure_log)
                print(f"Vectis log saved to {failure_log}", file=sys.stderr)
            raise
        finally:
            if process is not None:
                stop_proxy(process)
            origin.shutdown()
            origin.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
