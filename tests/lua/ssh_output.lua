local vectis = require("vectis")
local mode = assert(arg[3])
if mode:find("sftp", 1, true) == 1 then
  local ok, err = vectis.ssh.sftp_upload_file({
    host = "127.0.0.1", port = tonumber(arg[1]), host_key_sha256 = arg[2],
    username = "test", password = "test", local_path = arg[4],
    remote_path = "/upload", timeout_ms = 1000,
  })
  if mode == "sftp-close-failure" then
    assert(ok == nil and err, "failed CLOSE must not report success")
    assert(err.message:find("close remote SFTP upload", 1, true), err.message)
  else
    assert(ok, err and err.message)
  end
  return
end
local result, err = vectis.ssh.exec({
  host = "127.0.0.1",
  port = assert(tonumber(arg[1])),
  host_key_sha256 = assert(arg[2]),
  username = "test",
  password = "test",
  command = mode,
  timeout_ms = (mode == "idle" or mode == "completion-timeout") and 200 or 8000,
})
if mode == "idle" or mode == "completion-timeout" then
  assert(result == nil)
  assert(err.status == vectis.ERR_TIMEOUT, err.message)
  assert(err.message:find(mode == "idle" and "waiting for SSH output"
    or "SSH remote completion", 1, true), err.message)
else
  assert(result, err and err.message)
  assert(result.exit_status == 7)
  if mode == "stderr" then
    assert(result.stdout == "")
    assert(result.stderr == string.rep("e", 4 * 1024 * 1024))
  elseif mode == "mixed" then
    assert(result.stdout == string.rep("o", 2 * 1024 * 1024))
    assert(result.stderr == string.rep("e", 2 * 1024 * 1024))
  elseif mode == "delayed" then
    assert(result.stdout == "before")
    assert(result.stderr == "after")
  else
    assert(result.stdout == "" and result.stderr == "")
  end
end
