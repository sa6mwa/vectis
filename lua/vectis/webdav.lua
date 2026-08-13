local curl = require("curl")
local http = require("vectis.http")

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

local function ensure_headers(opts)
  if opts.headers == nil then
    opts.headers = {}
  end
  return opts.headers
end

local function set_header(opts, name, value)
  if value == nil then
    return
  end
  ensure_headers(opts)[name] = tostring(value)
end

local function default_protocols(opts)
  if opts.protocols == nil then
    opts.protocols = "http,https"
  end
end

local function normalize(result)
  return http.normalize(result)
end

local function request_with_method(method, first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or method
  return M.request(opts)
end

function M.request(opts)
  opts = opts_from(opts)
  if opts.url == nil then
    error("vectis.webdav.request requires url", 2)
  end
  default_protocols(opts)
  set_header(opts, "Depth", opts.depth)
  set_header(opts, "Destination", opts.destination)
  set_header(opts, "Overwrite", opts.overwrite)
  set_header(opts, "Authorization", opts.authorization)
  if opts.upload_path ~= nil or opts.body_path ~= nil then
    opts.upload = true
  end
  return normalize(curl.perform(opts))
end

function M.get(first, second)
  return request_with_method("GET", first, second)
end

function M.put(first, second)
  local opts = opts_from(first, second)
  if opts.body == nil and opts.body_path == nil and opts.upload_path == nil then
    error("vectis.webdav.put requires body, body_path, or upload_path", 2)
  end
  opts.method = opts.method or "PUT"
  return M.request(opts)
end

function M.delete(first, second)
  return request_with_method("DELETE", first, second)
end

function M.mkcol(first, second)
  return request_with_method("MKCOL", first, second)
end

function M.propfind(first, second)
  local opts = opts_from(first, second)
  opts.method = opts.method or "PROPFIND"
  if opts.depth == nil then
    opts.depth = "1"
  end
  return M.request(opts)
end

function M.copy(first, second)
  local opts = opts_from(first, second)
  if opts.destination == nil then
    error("vectis.webdav.copy requires destination", 2)
  end
  opts.method = opts.method or "COPY"
  return M.request(opts)
end

function M.move(first, second)
  local opts = opts_from(first, second)
  if opts.destination == nil then
    error("vectis.webdav.move requires destination", 2)
  end
  opts.method = opts.method or "MOVE"
  return M.request(opts)
end

function M.download(opts)
  opts = opts_from(opts)
  if opts.download_path == nil then
    error("vectis.webdav.download requires download_path", 2)
  end
  opts.method = opts.method or "GET"
  return M.request(opts)
end

function M.upload(opts)
  return M.put(opts)
end

M.error_for = http.error_for
M.normalize = normalize

return M
