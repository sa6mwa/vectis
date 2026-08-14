# Vectis Lua SSH

`vectis.ssh` exposes Vectis-owned libssh2-backed workflows. It is not a
dependency-native libssh2 session/channel binding.

## Command Execution

`vectis.ssh.exec(opts)` runs a command over SSH and returns captured stdout,
stderr, and exit status.

Required fields:

- `host`
- `username`
- `command`
- one of `password`, `private_key_path`, `key_path`, or `private_key`

Optional fields:

- `port`, defaulting to `22`
- `known_hosts_path` or `known_hosts`
- `timeout_ms`

## SFTP And SCP File Transfer

- `vectis.ssh.sftp_upload_file(opts)` uploads `local_path` to `remote_path`.
- `vectis.ssh.sftp_download_file(opts)` downloads `remote_path` to
  `local_path`.
- `vectis.ssh.scp_upload_file(opts)` uploads `local_path` to `remote_path`
  using SCP.
- `vectis.ssh.scp_download_file(opts)` downloads `remote_path` to
  `local_path` using SCP.

The SFTP and SCP helpers use the same SSH connection/auth fields as `exec`.

## SFTP Filesystem Operations

- `vectis.ssh.sftp_stat(opts)` stats `remote_path` and returns a table with
  `flags`, `has_size`, `size`, `size_overflow`, `has_uid_gid`, `uid`, `gid`,
  `has_permissions`, `permissions`, `has_atime`, `atime`, `has_mtime`, and
  `mtime`.
- `vectis.ssh.sftp_mkdir(opts)` creates `remote_path`. `permissions` defaults
  to decimal `493`, which is octal `0755`.
- `vectis.ssh.sftp_remove(opts)` removes the remote file at `remote_path`.
- `vectis.ssh.sftp_rmdir(opts)` removes the remote directory at `remote_path`.
- `vectis.ssh.sftp_rename(opts)` renames `old_path` to `new_path` using
  libssh2's overwrite/atomic/native rename mode.
- `vectis.ssh.sftp_chmod(opts)` updates `remote_path` permissions and requires
  `permissions`.

These are one-shot helpers: they open an SSH connection, authenticate, run the
SFTP operation, and close the session for each call.

## Stateful SFTP

`vectis.ssh.sftp_open(opts)` opens a reusable SFTP session with the same SSH
connection/auth fields as `exec`. It returns a session receiver. Close it with
`session:close()` when done; file and directory receivers also close on Lua GC.

Session methods:

- `session:open_file({ remote_path = path, mode = "r", permissions = 420 })`
  returns a file receiver. Supported modes are `r`, `w`, `a`, `r+`, `w+`,
  `a+`, and `rw`. Advanced users can pass numeric `flags` using the
  `VECTIS_SSH_SFTP_OPEN_*` C constants through libvectis.
- `session:open_dir({ remote_path = path })` returns a directory receiver.
- `session:stat({ remote_path = path })` returns the same stat table as
  `vectis.ssh.sftp_stat`.
- `session:mkdir`, `session:remove`, `session:rmdir`, `session:rename`, and
  `session:chmod` mirror the one-shot filesystem helpers.

File methods:

- `file:read([capacity])` returns the next chunk as a string. An empty string
  means EOF.
- `file:write(data)` writes the complete string and returns bytes written.
- `file:stat()` returns the same stat table as `vectis.ssh.sftp_stat`.
- `file:close()` closes the remote file handle.

Directory methods:

- `dir:read()` returns `{ name, long_name, stat }` for the next entry, or
  `nil` at EOF.
- `dir:close()` closes the remote directory handle.

Dependency-native libssh2 channel/session control and advanced host-key
workflows remain outside this helper surface.

```lua
local vectis = require("vectis")

local ok, err = vectis.ssh.sftp_upload_file({
  host = "example.test",
  username = "deploy",
  private_key_path = "deploy.key",
  known_hosts_path = "known_hosts",
  local_path = "site.tar",
  remote_path = "/srv/site.tar",
})
assert(ok, err and err.message)
```

```lua
local ok, err = vectis.ssh.scp_download_file({
  host = "example.test",
  username = "deploy",
  private_key_path = "deploy.key",
  known_hosts_path = "known_hosts",
  remote_path = "/srv/site.tar",
  local_path = "site.tar",
})
assert(ok, err and err.message)
```

```lua
local info, err = vectis.ssh.sftp_stat({
  host = "example.test",
  username = "deploy",
  private_key_path = "deploy.key",
  known_hosts_path = "known_hosts",
  remote_path = "/srv/site.tar",
})
assert(info, err and err.message)
if info.has_size then
  print(info.size)
end
```

```lua
local session = assert(vectis.ssh.sftp_open({
  host = "example.test",
  username = "deploy",
  private_key_path = "deploy.key",
  known_hosts_path = "known_hosts",
}))

local file = assert(session:open_file({
  remote_path = "/srv/site.tar",
  mode = "r",
}))
repeat
  local chunk = assert(file:read(65536))
  if #chunk > 0 then
    io.write(chunk)
  end
until #chunk == 0
file:close()

local dir = assert(session:open_dir({ remote_path = "/srv" }))
while true do
  local entry, read_err = dir:read()
  assert(entry ~= nil or read_err == nil, read_err and read_err.message)
  if entry == nil then
    break
  end
  print(entry.name)
end
dir:close()
session:close()
```

Failures return `nil, error`, where `error` is a structured Vectis status
table.
