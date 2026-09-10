"""Exercise Lua HTTP uploads on a local server (Python standard library only)."""

import email.parser
import email.policy
import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading

PAYLOAD = b"upload\x00bytes" * 4096


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle_upload(self):
        try:
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                chunks = []
                while True:
                    size = int(self.rfile.readline().split(b";", 1)[0], 16)
                    if size == 0:
                        assert self.rfile.readline() == b"\r\n"
                        break
                    chunks.append(self.rfile.read(size))
                    assert self.rfile.read(2) == b"\r\n"
                body = b"".join(chunks)
            else:
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if self.path.startswith("/json"):
                assert self.headers.get("Content-Length") is None
                assert self.headers.get("Transfer-Encoding", "").lower() == "chunked"
                assert json.loads(body) == {"payload": PAYLOAD.replace(b"\x00", b"_").decode()}
            elif self.path == "/multipart":
                mime = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                    b"Content-Type: " + self.headers["Content-Type"].encode()
                    + b"\r\nMIME-Version: 1.0\r\n\r\n" + body
                )
                parts = list(mime.iter_parts())
                assert len(parts) == 1
                assert parts[0].get_payload(decode=True) == PAYLOAD
                assert parts[0].get_param("name", header="content-disposition") == "payload"
            else:
                assert body == PAYLOAD
            self.server.requests.append((self.command, self.path))
            if self.path == "/json/retry" and self.server.requests.count((self.command, self.path)) == 1:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path.endswith(("/307", "/308")):
                self.send_response(int(self.path.rsplit("/", 1)[1]))
                self.send_header("Location", self.path.rsplit("/", 1)[0] + "/received")
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self.send_response(200)
                self.send_header("Content-Length", str(len(self.command)))
                self.end_headers()
                self.wfile.write(self.command.encode())
        except BaseException as error:
            self.server.errors.append((self.command, self.path, repr(error)))
            self.close_connection = True

    do_POST = do_PUT = do_PATCH = handle_upload


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    script = pathlib.Path(__file__).parent / "lua" / "http_uploads.lua"
    with tempfile.TemporaryDirectory(prefix="http-uploads-", dir=binary.parent) as work:
        path = pathlib.Path(work) / "payload.bin"
        path.write_bytes(PAYLOAD)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            server.requests = []
            server.errors = []
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                result = subprocess.run(
                    [str(binary), str(script.resolve()),
                     f"http://127.0.0.1:{server.server_port}", str(path)],
                    cwd=work, capture_output=True, text=True, timeout=45,
                )
            finally:
                server.shutdown()
                thread.join()
            assert not server.errors, server.errors
            assert result.returncode == 0, result.stdout + result.stderr
            for method in ("POST", "PUT", "PATCH"):
                for kind in ("file", "multipart", "raw", "json"):
                    assert (method, "/" + kind) in server.requests
                for kind in ("json", "raw", "file"):
                    assert server.requests.count((method, "/" + kind + "/received")) == 2
                assert server.requests.count((method, "/json/retry")) == 2
            assert len(server.requests) == 55, server.requests
            print("Passed 34 uploads, including 18 redirects and 3 retries (55 requests)")


if __name__ == "__main__":
    main()
