local curl = require("curl")

local M = {}

local function copy_table(source)
  local target = {}
  if source then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function opts_from(first, second)
  if type(first) == "table" then
    return copy_table(first)
  end
  local opts = copy_table(second)
  opts.url = first
  return opts
end

local function default_protocols(opts, protocols)
  if opts.protocols == nil then
    opts.protocols = protocols
  end
end

local function http_status_failed(status)
  return type(status) == "number" and status >= 400
end

function M.error_for(result)
  if type(result) ~= "table" then
    return {
      kind = "invalid_result",
      message = "HTTP result is invalid",
    }
  end
  if result.ok ~= true then
    return {
      kind = "transport",
      code = result.code,
      code_name = result.code_name,
      message = result.error or result.code_name or "curl transfer failed",
      attempts = result.attempts,
    }
  end
  if http_status_failed(result.status) then
    return {
      kind = "http_status",
      status = result.status,
      message = "HTTP request failed with status " .. tostring(result.status),
      body = result.body,
      json = result.json,
      attempts = result.attempts,
    }
  end
  return nil
end

function M.normalize(result)
  local transport_ok = type(result) == "table" and result.ok == true
  local error_info = M.error_for(result)

  if type(result) ~= "table" then
    result = {}
  end
  result.transport_ok = transport_ok
  if error_info ~= nil then
    if type(result.error) == "string" then
      result.error_message = result.error
    end
    result.ok = false
    result.error = error_info
  else
    result.ok = true
    result.error = nil
  end
  return result
end

function M.request(opts)
  opts = opts_from(opts)
  default_protocols(opts, "http,https")
  return M.normalize(curl.perform(opts))
end

function M.request_json(opts)
  opts = opts_from(opts)
  default_protocols(opts, "http,https")
  return M.normalize(curl.json(opts))
end

function M.get_json(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "GET"
  return M.request_json(opts)
end

function M.post_json(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "POST"
  return M.request_json(opts)
end

function M.put_json(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "PUT"
  return M.request_json(opts)
end

function M.patch_json(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "PATCH"
  return M.request_json(opts)
end

function M.delete_json(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "DELETE"
  return M.request_json(opts)
end

function M.download(opts)
  opts = opts_from(opts)
  if opts.download_path == nil then
    error("vectis.http.download requires download_path", 2)
  end
  return M.request(opts)
end

function M.upload(opts)
  opts = opts_from(opts)
  if opts.upload_path == nil and opts.body_path == nil and opts.body == nil then
    error("vectis.http.upload requires upload_path, body_path, or body", 2)
  end
  if opts.upload_path ~= nil or opts.body_path ~= nil then
    opts.upload = true
  end
  return M.request(opts)
end

function M.sftp_download(opts)
  opts = opts_from(opts)
  default_protocols(opts, "sftp")
  return M.download(opts)
end

function M.sftp_upload(opts)
  opts = opts_from(opts)
  default_protocols(opts, "sftp")
  opts.upload = true
  return M.upload(opts)
end

function M.stream_json(opts)
  opts = opts_from(opts)
  default_protocols(opts, "http,https")
  return M.normalize(curl.stream_json(opts))
end

return M
