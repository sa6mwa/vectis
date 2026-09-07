local vectis = require("vectis")
local mode = assert(arg[3])
local result, err = vectis.ssh.exec({
  host = "127.0.0.1",
  port = assert(tonumber(arg[1])),
  host_key_sha256 = assert(arg[2]),
  username = "test",
  password = "test",
  command = mode,
  timeout_ms = mode == "idle" and 200 or 8000,
})
if mode == "idle" then
  assert(result == nil)
  assert(err.status == vectis.ERR_TIMEOUT, err.message)
  assert(err.message:find("waiting for SSH output", 1, true))
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
