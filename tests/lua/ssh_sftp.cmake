set(upload_path "${WORK_DIR}/vectis-ssh-sftp-upload.txt")
set(download_path "${WORK_DIR}/vectis-ssh-sftp-download.txt")
set(script "${WORK_DIR}/vectis-ssh-sftp.lua")

file(WRITE "${upload_path}" "vectis lua sftp upload\n")
file(REMOVE "${download_path}")

file(WRITE "${script}" [[
local vectis = require("vectis")

local upload_path = assert(arg[1])
local download_path = assert(arg[2])

assert(type(vectis.ssh.sftp_upload_file) == "function")
assert(type(vectis.ssh.sftp_download_file) == "function")
assert(type(vectis.ssh.scp_upload_file) == "function")
assert(type(vectis.ssh.scp_download_file) == "function")
assert(type(vectis.ssh.sftp_stat) == "function")
assert(type(vectis.ssh.sftp_mkdir) == "function")
assert(type(vectis.ssh.sftp_remove) == "function")
assert(type(vectis.ssh.sftp_rmdir) == "function")
assert(type(vectis.ssh.sftp_rename) == "function")
assert(type(vectis.ssh.sftp_chmod) == "function")

local missing_local, missing_local_err = vectis.ssh.sftp_upload_file({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  remote_path = "/tmp/upload.txt",
})
assert(missing_local == nil)
assert(type(missing_local_err) == "table")
assert(missing_local_err.status == vectis.ERR_INVALID)
assert(missing_local_err.message:find("local_path", 1, true))

local bad_port, bad_port_err = vectis.ssh.sftp_download_file({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  remote_path = "/tmp/upload.txt",
  local_path = download_path,
  port = 70000,
})
assert(bad_port == nil)
assert(type(bad_port_err) == "table")
assert(bad_port_err.status == vectis.ERR_INVALID)
assert(bad_port_err.message:find("port", 1, true))

local missing_scp_remote, missing_scp_remote_err = vectis.ssh.scp_upload_file({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  local_path = upload_path,
})
assert(missing_scp_remote == nil)
assert(type(missing_scp_remote_err) == "table")
assert(missing_scp_remote_err.status == vectis.ERR_INVALID)
assert(missing_scp_remote_err.message:find("remote_path", 1, true))

local missing_stat_path, missing_stat_path_err = vectis.ssh.sftp_stat({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
})
assert(missing_stat_path == nil)
assert(type(missing_stat_path_err) == "table")
assert(missing_stat_path_err.status == vectis.ERR_INVALID)
assert(missing_stat_path_err.message:find("remote_path", 1, true))

local bad_chmod_mode, bad_chmod_mode_err = vectis.ssh.sftp_chmod({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  remote_path = "/tmp/upload.txt",
  permissions = 010000,
})
assert(bad_chmod_mode == nil)
assert(type(bad_chmod_mode_err) == "table")
assert(bad_chmod_mode_err.status == vectis.ERR_INVALID)
assert(bad_chmod_mode_err.message:find("permissions", 1, true))

local missing_rename_path, missing_rename_path_err = vectis.ssh.sftp_rename({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  old_path = "/tmp/old.txt",
})
assert(missing_rename_path == nil)
assert(type(missing_rename_path_err) == "table")
assert(missing_rename_path_err.status == vectis.ERR_INVALID)
assert(missing_rename_path_err.message:find("new_path", 1, true))

local refused, refused_err = vectis.ssh.sftp_upload_file({
  host = "127.0.0.1",
  port = 1,
  username = "vectis",
  password = "secret",
  local_path = upload_path,
  remote_path = "/tmp/vectis-upload.txt",
  timeout_ms = 200,
})
assert(refused == nil)
assert(type(refused_err) == "table")
assert(type(refused_err.status_string) == "string")
assert(type(refused_err.message) == "string")
assert(#refused_err.message > 0)

local stat_refused, stat_refused_err = vectis.ssh.sftp_stat({
  host = "127.0.0.1",
  port = 1,
  username = "vectis",
  password = "secret",
  remote_path = "/tmp/vectis-upload.txt",
  timeout_ms = 200,
})
assert(stat_refused == nil)
assert(type(stat_refused_err) == "table")
assert(type(stat_refused_err.status_string) == "string")
assert(type(stat_refused_err.message) == "string")
assert(#stat_refused_err.message > 0)

local scp_refused, scp_refused_err = vectis.ssh.scp_upload_file({
  host = "127.0.0.1",
  port = 1,
  username = "vectis",
  password = "secret",
  local_path = upload_path,
  remote_path = "/tmp/vectis-upload.txt",
  timeout_ms = 200,
})
assert(scp_refused == nil)
assert(type(scp_refused_err) == "table")
assert(type(scp_refused_err.status_string) == "string")
assert(type(scp_refused_err.message) == "string")
assert(#scp_refused_err.message > 0)

local scp_download_refused, scp_download_refused_err = vectis.ssh.scp_download_file({
  host = "127.0.0.1",
  port = 1,
  username = "vectis",
  password = "secret",
  remote_path = "/tmp/vectis-upload.txt",
  local_path = download_path,
  timeout_ms = 200,
})
assert(scp_download_refused == nil)
assert(type(scp_download_refused_err) == "table")
assert(type(scp_download_refused_err.message) == "string")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${upload_path}"
                        "${download_path}"
                RESULT_VARIABLE ssh_sftp_result
                OUTPUT_VARIABLE ssh_sftp_stdout
                ERROR_VARIABLE ssh_sftp_stderr)
if(NOT ssh_sftp_result EQUAL 0)
  message(FATAL_ERROR "vectis SSH/SFTP Lua smoke failed: ${ssh_sftp_stdout}${ssh_sftp_stderr}")
endif()
