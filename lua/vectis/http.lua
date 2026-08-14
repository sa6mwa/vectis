local curl = require("curl")
local vstatus = require("vectis.status")

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

local function curl_status(result)
  if result and (result.code == 28 or
      result.code_name == "CURLE_OPERATION_TIMEDOUT") then
    return vstatus.ERR_TIMEOUT
  end
  return vstatus.ERR_STATE
end

local function request_with_method(method, first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or method
  return M.request(opts)
end

local function merge_headers(default_headers, request_headers)
  local headers = nil
  if default_headers ~= nil then
    headers = copy_table(default_headers)
  end
  if request_headers ~= nil then
    headers = headers or {}
    for key, value in pairs(request_headers) do
      headers[key] = value
    end
  end
  return headers
end

local function merge_defaults(defaults, first, second)
  local request = opts_from(first, second)
  local merged = copy_table(defaults)
  for key, value in pairs(request) do
    if key ~= "headers" then
      merged[key] = value
    end
  end
  merged.headers = merge_headers(defaults and defaults.headers or nil,
                                 request.headers)
  return merged
end

local function percent_encode(value)
  value = tostring(value)
  return (value:gsub("([^A-Za-z0-9_%.%-~])", function(char)
    return string.format("%%%02X", string.byte(char))
  end))
end

local function append_form_pair(out, key, value)
  out[#out + 1] = percent_encode(key) .. "=" .. percent_encode(value)
end

local function header_value(headers, name)
  local wanted = name:lower()
  if type(headers) ~= "table" then
    return nil
  end
  for key, value in pairs(headers) do
    if type(key) == "string" and key:lower() == wanted then
      return value
    end
  end
  return nil
end

function M.error_for(result)
  if type(result) ~= "table" then
    return vstatus.error({
      kind = "invalid_result",
      message = "HTTP result is invalid",
      status = vstatus.ERR_INVALID,
      source_code = vstatus.ERROR_SOURCE_VECTIS,
    })
  end
  if result.ok ~= true then
    return vstatus.error({
      kind = "transport",
      code = result.code,
      code_name = result.code_name,
      message = result.error or result.code_name or "curl transfer failed",
      attempts = result.attempts,
      status = curl_status(result),
      source_code = vstatus.ERROR_SOURCE_CURL,
      dependency_code = result.code,
    })
  end
  if http_status_failed(result.status) then
    return vstatus.error({
      kind = "http_status",
      message = "HTTP request failed with status " .. tostring(result.status),
      body = result.body,
      json = result.json,
      attempts = result.attempts,
      status = vstatus.ERR_STATE,
      source_code = vstatus.ERROR_SOURCE_CURL,
      http_status = result.status,
    })
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

function M.get(first, second)
  return request_with_method("GET", first, second)
end

function M.post(first, second)
  return request_with_method("POST", first, second)
end

function M.put(first, second)
  return request_with_method("PUT", first, second)
end

function M.patch(first, second)
  return request_with_method("PATCH", first, second)
end

function M.delete(first, second)
  return request_with_method("DELETE", first, second)
end

function M.head(first, second)
  return request_with_method("HEAD", first, second)
end

function M.options(first, second)
  return request_with_method("OPTIONS", first, second)
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

function M.form_encode(form)
  local out = {}
  if type(form) ~= "table" then
    error("vectis.http.form_encode requires a table", 2)
  end
  for key, value in pairs(form) do
    if type(value) == "table" then
      for _, item in ipairs(value) do
        append_form_pair(out, key, item)
      end
    elseif value ~= nil then
      append_form_pair(out, key, value)
    end
  end
  table.sort(out)
  return table.concat(out, "&")
end

function M.form(opts)
  opts = opts_from(opts)
  if opts.form == nil then
    error("vectis.http.form requires form", 2)
  end
  opts.body = M.form_encode(opts.form)
  opts.method = opts.method or "POST"
  opts.headers = copy_table(opts.headers)
  if header_value(opts.headers, "content-type") == nil then
    opts.headers["Content-Type"] = "application/x-www-form-urlencoded"
  end
  return M.request(opts)
end

function M.multipart(opts)
  opts = opts_from(opts)
  if opts.multipart == nil then
    opts.multipart = opts.parts
    opts.parts = nil
  end
  if opts.multipart == nil then
    error("vectis.http.multipart requires multipart or parts", 2)
  end
  opts.method = opts.method or "POST"
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

function M.client(defaults)
  defaults = copy_table(defaults)
  local client = {}

  function client.request(first, second)
    return M.request(merge_defaults(defaults, first, second))
  end

  function client.request_json(first, second)
    return M.request_json(merge_defaults(defaults, first, second))
  end

  function client.get(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "GET"
    return M.request(opts)
  end

  function client.post(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "POST"
    return M.request(opts)
  end

  function client.put(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "PUT"
    return M.request(opts)
  end

  function client.patch(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "PATCH"
    return M.request(opts)
  end

  function client.delete(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "DELETE"
    return M.request(opts)
  end

  function client.head(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "HEAD"
    return M.request(opts)
  end

  function client.options(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "OPTIONS"
    return M.request(opts)
  end

  function client.get_json(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "GET"
    return M.request_json(opts)
  end

  function client.post_json(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "POST"
    return M.request_json(opts)
  end

  function client.put_json(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "PUT"
    return M.request_json(opts)
  end

  function client.patch_json(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "PATCH"
    return M.request_json(opts)
  end

  function client.delete_json(first, second)
    local opts = merge_defaults(defaults, first, second)
    opts.method = opts.method or "DELETE"
    return M.request_json(opts)
  end

  function client.download(first, second)
    return M.download(merge_defaults(defaults, first, second))
  end

  function client.upload(first, second)
    return M.upload(merge_defaults(defaults, first, second))
  end

  function client.form(first, second)
    return M.form(merge_defaults(defaults, first, second))
  end

  function client.multipart(first, second)
    return M.multipart(merge_defaults(defaults, first, second))
  end

  function client.sftp_download(first, second)
    return M.sftp_download(merge_defaults(defaults, first, second))
  end

  function client.sftp_upload(first, second)
    return M.sftp_upload(merge_defaults(defaults, first, second))
  end

  function client.stream_json(first, second)
    return M.stream_json(merge_defaults(defaults, first, second))
  end

  return client
end

return M
