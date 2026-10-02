#!/usr/bin/env python3
"""Exercise real CLI and MCP error recovery without polling or sleeps."""
import argparse
import http.client
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tempfile
import time


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, timeout=5, **kwargs)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result


def descendants(pid):
    """Only inspect this test child's tree; Kore can create another session."""
    try:
        children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
    except FileNotFoundError:
        return set()
    result = {int(child) for child in children}
    for child in tuple(result):
        result.update(descendants(child))
    return result


def readiness(process):
    deadline = time.monotonic() + 3
    protocol, serving, pending = None, False, b""
    with selectors.DefaultSelector() as ready:
        ready.register(process.stdout, selectors.EVENT_READ)
        while not (protocol and serving):
            remaining = deadline - time.monotonic()
            assert remaining > 0 and ready.select(timeout=remaining), "missing readiness event"
            data = os.read(process.stdout.fileno(), 4096)
            assert data, f"server exited before readiness: {process.poll()}"
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if line.startswith(b"PROTOCOL "):
                    protocol = line.decode().split(" ", 1)[1]
                elif line.startswith(b"{"):
                    serving |= "serving http on" in json.loads(line).get("msg", "")
    return protocol


def mcp(command, topology, work):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with (work / (topology + ".log")).open("wb") as log:
        process = subprocess.Popen(
            [*command, str(port), topology], stdout=subprocess.PIPE,
            stderr=log, start_new_session=True,
        )
        connection = None
        try:
            protocol = readiness(process)
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            headers = {"Accept": "application/json, text/event-stream",
                       "Content-Type": "application/json", "MCP-Protocol-Version": protocol}
            checks = 0

            def request(payload, extra=None):
                data = payload if isinstance(payload, str) else json.dumps(payload)
                connection.request("POST", "/mcp", data, {**headers, **(extra or {})})
                response = connection.getresponse()
                body = response.read().decode()
                assert response.status != 503, (payload, response.getheaders(), body)
                if response.status == 413:
                    assert response.getheader("Connection", "").lower() == "close", response.getheaders()
                content_type = response.getheader("Content-Type", "")
                if content_type.startswith("text/event-stream"):
                    body = [line[6:] for line in body.splitlines() if line.startswith("data: ")][-1]
                elif "json" not in content_type:
                    return response.status, body
                try:
                    return response.status, json.loads(body) if body else None
                except ValueError as error:
                    raise AssertionError((payload, response.status, body)) from error

            def call(name, arguments=None):
                return request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": name, "arguments": arguments or {}}})

            def good(name="good"):
                nonlocal checks
                status, value = call(name)
                assert status == 200 and value["result"]["structuredContent"] == {"ok": True}, (status, value)
                checks += 1

            status, value = request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                     "params": {"protocolVersion": protocol, "capabilities": {},
                                                "clientInfo": {"name": "failure-tests", "version": "1"}}})
            assert status == 200 and value["result"]["protocolVersion"] == protocol, value
            checks += 1
            good()
            for name in ("throws", "nil_result", "nil_message", "table_error",
                         "wrong_type", "invalid_json", "oversized", "missing_tool"):
                status, value = call(name)
                if status == 500 and name in ("invalid_json", "oversized"):
                    assert isinstance(value, str) and "MCP" in value, (name, value)
                else:
                    assert status == 200 and (value.get("error") or value.get("result", {}).get("isError")), (name, value)
                if name in ("throws", "nil_message"):
                    assert "expected callback" in json.dumps(value), value
                checks += 1
                good()  # A healthy call after every failure proves stack/lock recovery.
            status, value = call("good", {"unexpected": True})
            assert status == 200 and (value.get("error") or value.get("result", {}).get("isError")), value
            checks += 1
            good()
            for payload in ("{invalid", '{"jsonrpc":"2.0","id":1,"method":"missing"}',
                            "x" * 4096):
                status, value = request(payload)
                assert status >= 400 or value.get("error"), (status, value)
                checks += 1
                good()
            # Reject an unfinished body without waiting for its remaining bytes.
            connection.putrequest("POST", "/mcp")
            connection.putheader("Content-Type", "application/json")
            connection.putheader("Content-Length", "4096")
            connection.endheaders(b"x" * 2049)
            response = connection.getresponse()
            assert response.status == 413, response.status
            assert response.getheader("Connection", "").lower() == "close"
            response.read()
            checks += 1
            good()
            status, value = request('{"jsonrpc":"2.0","id":1,"method":"tools/list"}',
                                    {"Origin": "https://untrusted.example"})
            assert status in (400, 403), (status, value)
            checks += 1
            good("collecting")
            good()
        finally:
            if connection is not None:
                connection.close()
            owned = descendants(process.pid)
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                for pid in owned | {process.pid}:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.stdout.close()
                process.wait(timeout=1)
                raise
        assert process.returncode == 0 and b"mcp failure cleanup ok" in output, (
            process.returncode, output, (work / (topology + ".log")).read_text(errors="replace"))
        print(f"{topology}: {checks + 1} MCP failure/recovery checks passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    source = Path(__file__).resolve().parent
    args.work_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(dir=args.work_dir, prefix="failure-modes-") as directory:
        work = Path(directory)
        # lua_close must finalize a live app even after an uncaught Lua error.
        script = work / "uncaught.lua"
        script.write_text('local v=require("vectis")\n'
                          'local app=assert(v.app.new({access_log=false,port=39099}))\n'
                          'assert(app:mcp({tools={{name="echo",schema_json="{}",callback=function() return "{}" end}}}))\n'
                          'error("expected top-level failure",0)\n')
        packed = work / "uncaught-packed"
        run([str(binary), "-a", "pack", "-s", str(script), "-o", str(packed)])
        for command in ([str(binary), str(script)], [str(packed)]):
            result = subprocess.run(command, capture_output=True, timeout=3)
            assert result.returncode == 1 and b"expected top-level failure" in result.stderr, (
                command, result.returncode, result.stderr)
        print("script/packed: uncaught error finalization passed")
        server = source / "mcp_failure_server.lua"
        mcp([str(binary), str(server)], "direct", work)
        mcp([str(binary), str(server)], "supervised", work)
        packed = work / "mcp-packed"
        run([str(binary), "-a", "pack", "-s", str(server), "-o", str(packed)])
        mcp([str(packed)], "supervised", work)
    print(f"failure modes completed in {time.monotonic() - started:.3f}s")


if __name__ == "__main__":
    main()
