#!/usr/bin/env python3
"""Verify Lua MQTT publications with a tiny loopback MQTT 3.1.1 peer."""
import pathlib
import socket
import subprocess
import sys
import tempfile


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise AssertionError("MQTT peer closed before completing packet")
        data.extend(chunk)
    return bytes(data)


def receive_packet(connection):
    kind = receive_exact(connection, 1)[0]
    size = 0
    for index in range(4):
        byte = receive_exact(connection, 1)[0]
        size += (byte & 127) << (7 * index)
        if byte < 128:
            assert size <= 1024 * 1024, "test peer packet limit exceeded"
            return kind, receive_exact(connection, size)
    raise AssertionError("invalid MQTT remaining length")


def main():
    binary, work = sys.argv[1:]
    payloads = [b"body", b"payload", b"message", b"binary\x00tail", b"", b"x" * 16384]
    with tempfile.TemporaryDirectory(prefix="mqtt-wire-", dir=work) as directory:
        root = pathlib.Path(directory)
        script = root / "publish.lua"
        script.write_text('''
local mqtt = require("vectis.mqtt")
local url = "mqtt://127.0.0.1:" .. arg[1] .. "/vectis/wire"
local cases = {
  {body = "body"}, {payload = "payload"}, {message = "message"},
  {body = "binary\\0tail"}, {body = ""},
  {body = ("x"):rep(16384), method = "GET", upload = true},
}
for index, opts in ipairs(cases) do
  opts.url = url
  opts.timeout_ms = 2000
  opts.connect_timeout_ms = 1000
  opts.retry = false
  opts.proxy = ""
  local result
  if index == 3 then
    opts.url = nil
    result = mqtt.publish(url, opts)
  else
    result = mqtt.publish(opts)
  end
  assert(result.ok, result.error and result.error.message)
  assert(result.transport_ok)
end
print("MQTT publications verified")
''')
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            port = listener.getsockname()[1]
            with (root / "stdout").open("w+") as stdout, (root / "stderr").open("w+") as stderr:
                child = subprocess.Popen([binary, str(script), str(port)], stdout=stdout, stderr=stderr)
                try:
                    for expected in payloads:
                        connection, _ = listener.accept()
                        with connection:
                            connection.settimeout(5)
                            kind, connect = receive_packet(connection)
                            assert kind == 0x10 and connect.startswith(b"\x00\x04MQTT\x04"), (kind, connect)
                            connection.sendall(b"\x20\x02\x00\x00")
                            kind, publish = receive_packet(connection)
                            assert kind == 0x30, f"expected PUBLISH, received {kind:#x}"
                            topic_size = int.from_bytes(publish[:2], "big")
                            assert publish[2:2 + topic_size] == b"vectis/wire"
                            assert publish[2 + topic_size:] == expected
                            assert receive_packet(connection) == (0xE0, b"")
                    assert child.wait(timeout=5) == 0
                except BaseException:
                    child.kill()
                    child.wait()
                    stdout.seek(0)
                    stderr.seek(0)
                    print(stdout.read(), stderr.read(), file=sys.stderr)
                    raise
    print("MQTT wire regression passed: six payloads, PUBLISH and DISCONNECT")


if __name__ == "__main__":
    main()
