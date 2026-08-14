# Vectis Lua SSH

`vectis.ssh` exposes Vectis-owned libssh2-backed workflows. It is not a raw
libssh2 session/channel binding.

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
  to `0755`.
- `vectis.ssh.sftp_remove(opts)` removes the remote file at `remote_path`.
- `vectis.ssh.sftp_rmdir(opts)` removes the remote directory at `remote_path`.
- `vectis.ssh.sftp_rename(opts)` renames `old_path` to `new_path` using
  libssh2's overwrite/atomic/native rename mode.
- `vectis.ssh.sftp_chmod(opts)` updates `remote_path` permissions and requires
  `permissions`.

These are one-shot helpers: they open an SSH connection, authenticate, run the
SFTP operation, and close the session for each call. Stateful remote file
handles, directory iterators, and raw channel/session control remain outside
this helper surface until their Lua ownership contract is defined.

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

Failures return `nil, error`, where `error` is a structured Vectis status
table.
