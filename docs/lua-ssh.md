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

Failures return `nil, error`, where `error` is a structured Vectis status
table.
