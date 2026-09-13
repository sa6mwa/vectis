"""Local SSH regression: python3 tests/ssh_output.py build/debug/vectis.

Requires the test-only Paramiko package. No SSH daemon or system config needed.
"""

import hashlib
import pathlib
import socket
import subprocess
import sys
import threading
import time
import traceback

import paramiko
from paramiko.sftp import CMD_CLOSE


class UploadHandle(paramiko.SFTPHandle):
    def write(self, offset, data):
        return paramiko.SFTP_OK


class UploadServer(paramiko.SFTPServerInterface):
    def open(self, path, flags, attr):
        return UploadHandle()


class CloseFailureServer(paramiko.SFTPServer):
    def _process(self, packet_type, request_number, message):
        if packet_type == CMD_CLOSE:
            handle = message.get_binary()
            self.file_table.pop(handle).close()
            self._send_status(request_number, paramiko.SFTP_FAILURE)
            return
        super()._process(packet_type, request_number, message)


class Server(paramiko.ServerInterface):
    def __init__(self):
        self.ready = threading.Event()
        self.mode = None

    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL

    def check_channel_request(self, kind, channel_id):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else 1

    def check_channel_exec_request(self, channel, command):
        self.mode = command.decode("ascii")
        self.ready.set()
        return True


def serve(listener, key, errors, mode):
    try:
        connection, _ = listener.accept()
        with paramiko.Transport(connection) as transport:
            transport.add_server_key(key)
            if mode.startswith("sftp"):
                transport.set_subsystem_handler(
                    "sftp", CloseFailureServer if mode == "sftp-close-failure"
                    else paramiko.SFTPServer, UploadServer)
            server = Server()
            transport.start_server(server=server)
            channel = transport.accept(10)
            if mode.startswith("sftp"):
                deadline = time.monotonic() + 10
                while transport.is_active() and time.monotonic() < deadline:
                    time.sleep(0.01)
                return
            assert channel is not None and server.ready.wait(10)
            channel.settimeout(10)
            if server.mode == "stderr":
                channel.sendall_stderr(b"e" * (4 * 1024 * 1024))
            elif server.mode == "mixed":
                for _ in range(64):
                    channel.sendall_stderr(b"e" * 32768)
                    channel.sendall(b"o" * 32768)
            elif server.mode == "delayed":
                channel.sendall(b"before")
                time.sleep(0.2)
                channel.sendall_stderr(b"after")
            elif server.mode in ("idle", "completion-timeout"):
                if server.mode == "completion-timeout":
                    channel.shutdown_write()
                # Respond to the client's close after its inactivity deadline.
                deadline = time.monotonic() + 3
                while not channel.closed and time.monotonic() < deadline:
                    time.sleep(0.01)
                return
            if server.mode == "late-status":
                channel.shutdown_write()
                time.sleep(0.2)
            channel.send_exit_status(7)
            channel.shutdown_write()
            channel.close()
            deadline = time.monotonic() + 3
            while transport.is_active() and time.monotonic() < deadline:
                time.sleep(0.01)
    except BaseException:
        errors.append((mode, traceback.format_exc()))


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    script = pathlib.Path(__file__).parent / "lua" / "ssh_output.lua"
    key = paramiko.RSAKey.generate(2048)
    fingerprint = hashlib.sha256(key.asbytes()).hexdigest()
    for mode in ("stderr", "mixed", "delayed", "empty", "idle", "late-status",
                 "completion-timeout", "sftp-close-failure", "sftp-ok"):
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(15)
            thread = threading.Thread(target=serve, args=(listener, key, errors, mode))
            thread.start()
            started = time.monotonic()
            try:
                result = subprocess.run(
                    [str(binary), str(script.resolve()),
                     str(listener.getsockname()[1]), fingerprint, mode, str(script.resolve())],
                    cwd=binary.parent, capture_output=True, text=True, timeout=12,
                )
                elapsed = time.monotonic() - started
            finally:
                thread.join(15)
            assert not thread.is_alive(), "SSH fixture did not stop"
            assert not errors, (errors, result.returncode, result.stdout, result.stderr)
            assert result.returncode == 0, (mode, result.stdout + result.stderr)
            assert elapsed < 4, (mode, elapsed)
            print(f"{mode}: passed in {elapsed:.3f}s")


if __name__ == "__main__":
    main()
