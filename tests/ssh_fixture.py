"""Serve local SSH/SFTP wire inputs only; Bash/Lua own test execution/assertions.

Test-only Paramiko is optional. No daemon or system configuration is needed.
"""
import hashlib
import socket
import sys

import paramiko


class UploadHandle(paramiko.SFTPHandle):
    def __init__(self, fail_close):
        super().__init__()
        self.fail_close = fail_close

    def write(self, offset, data):
        return paramiko.SFTP_OK


class UploadServer(paramiko.SFTPServerInterface):
    def open(self, path, flags, attr):
        return UploadHandle(path == "/upload-close-failure")


class UploadProtocol(paramiko.SFTPServer):
    def _process(self, packet_type, request_number, message):
        if packet_type == paramiko.sftp.CMD_CLOSE:
            handle = self.file_table.pop(message.get_binary())
            handle.close()
            self._send_status(request_number, paramiko.SFTP_FAILURE
                              if handle.fail_close else paramiko.SFTP_OK)
            return
        super()._process(packet_type, request_number, message)


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL

    def check_channel_request(self, kind, channel_id):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else 1

    def check_channel_exec_request(self, channel, command):
        mode = command.decode("ascii")
        # A transport callback must not block on a channel window. Its data
        # producer runs in a separate thread owned by this fixture connection.
        import threading
        threading.Thread(target=send_fixture, args=(channel, mode), daemon=True).start()
        return True


def read_exact(channel, size):
    while size:
        chunk = channel.recv(min(size, 32768))
        if not chunk:
            return False
        size -= len(chunk)
    return True


def send_fixture(channel, mode):
    try:
        if mode.startswith("scp -t "):
            channel.sendall(b"\0")
            header = b""
            while not header.endswith(b"\n") and len(header) < 4096:
                chunk = channel.recv(1)
                if not chunk:
                    return
                header += chunk
            fields = header.rstrip(b"\n").split(b" ", 2)
            if len(fields) != 3 or fields[0] != b"C0644":
                channel.sendall(b"\2Invalid fixture upload\n")
                return
            channel.sendall(b"\0")
            if not read_exact(channel, int(fields[1])):
                return
            channel.sendall(b"\1No space left on device\n")
            channel.send_exit_status(1)
        else:
            if mode == "stderr":
                for _ in range(128):
                    channel.sendall_stderr(b"e" * 32768)
            elif mode == "mixed":
                for _ in range(64):
                    channel.sendall_stderr(b"e" * 32768)
                    channel.sendall(b"o" * 32768)
            elif mode == "split-output":
                channel.sendall(b"before")
                channel.sendall_stderr(b"after")
            elif mode == "stdin-eof":
                if channel.recv(1) == b"":
                    channel.sendall(b"eof")
            elif mode in ("idle", "completion-timeout"):
                if mode == "completion-timeout":
                    channel.shutdown_write()
                channel.status_event.wait(10)
                return
            if mode == "eof-before-status":
                channel.shutdown_write()
            channel.send_exit_status(7)
        channel.shutdown_write()
        channel.close()
    except (EOFError, OSError):
        pass  # Client failure/timeout closes the transport; Lua reports it.


key = paramiko.RSAKey.generate(2048)
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    with open(sys.argv[1], "w") as ready:
        ready.write(str(listener.getsockname()[1]) + " " +
                    hashlib.sha256(key.asbytes()).hexdigest() + "\n")
    while True:
        connection, _ = listener.accept()
        try:
            with paramiko.Transport(connection) as transport:
                transport.add_server_key(key)
                transport.set_subsystem_handler("sftp", UploadProtocol, UploadServer)
                transport.start_server(server=Server())
                # Retain the accepted channel: Transport keeps weak references.
                channel = transport.accept(10)
                transport.join(12)
        except (EOFError, OSError, paramiko.SSHException):
            connection.close()
