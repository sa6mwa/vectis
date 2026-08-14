local http = require("vectis.http")
local lonejson = require("lonejson")
local vstatus = require("vectis.status")

local M = {}

M.JSON_CONTENT_TYPE = "application/json; charset=utf-8"

local route_only_keys = {
  decode_json = true,
  error_handler = true,
  max_body_bytes = true,
  response = true,
  routes = true,
  validate = true,
}

local function copy_table(source)
  local target = {}
  if source then
    for key, value in pairs(source) do
      target[key] = value
    end
  end
  return target
end

local function copy_server_route_options(opts)
  local route = {}
  for key, value in pairs(opts) do
    if not route_only_keys[key] then
      route[key] = value
    end
  end
  return route
end

local function merge_defaults(defaults, opts)
  local merged = copy_table(defaults)
  if opts then
    for key, value in pairs(opts) do
      merged[key] = value
    end
  end
  return merged
end

local function with_json_content_type(headers)
  headers = copy_table(headers)
  for key in pairs(headers) do
    if type(key) == "string" and key:lower() == "content-type" then
      return headers
    end
  end
  headers["Content-Type"] = M.JSON_CONTENT_TYPE
  return headers
end

local function encode_json(value)
  local ok, encoded = pcall(lonejson.encode_json, value)
  if ok then
    return encoded
  end
  return nil, vstatus.error({
    kind = "json_encode",
    message = tostring(encoded),
    status = vstatus.ERR_INVALID,
    source_code = vstatus.ERROR_SOURCE_LONEJSON,
  })
end

local function route_response(value)
  if type(value) ~= "table" then
    return false
  end
  return value.body ~= nil or value.file_path ~= nil or value.file ~= nil or
      value.content_type ~= nil or value.headers ~= nil or
      type(value.status) == "number" or type(value.status_code) == "number"
end

local function http_status_for_error(err)
  if type(err) == "table" and type(err.http_status) == "number" then
    return err.http_status
  end
  local status = type(err) == "table" and err.status or nil
  if status == vstatus.ERR_INVALID then
    return 400
  elseif status == vstatus.ERR_CONFLICT then
    return 409
  elseif status == vstatus.ERR_NOT_IMPLEMENTED then
    return 501
  elseif status == vstatus.ERR_TIMEOUT then
    return 504
  end
  return 500
end

local function static_error_response(status_code)
  return {
    status = status_code or 500,
    content_type = M.JSON_CONTENT_TYPE,
    body = '{"ok":false,"error":{"kind":"json_encode",' ..
        '"message":"REST error response JSON encode failed",' ..
        '"status":1,"status_string":"invalid",' ..
        '"source":"lonejson","source_code":4}}\n',
  }
end

function M.json(value, opts)
  opts = opts or {}
  local body, err = encode_json(value)
  if body == nil then
    return nil, err
  end
  if opts.newline ~= false then
    body = body .. "\n"
  end
  return {
    status = opts.status or opts.status_code or 200,
    content_type = opts.content_type or M.JSON_CONTENT_TYPE,
    headers = opts.headers,
    body = body,
  }
end

function M.error_response(err, opts)
  opts = opts or {}
  err = vstatus.decorate_error(err, {
    status = opts.error_status or vstatus.ERR_STATE,
    source_code = opts.source_code or vstatus.ERROR_SOURCE_VECTIS,
    http_status = opts.status or opts.status_code,
  })
  local status_code = opts.status or opts.status_code or http_status_for_error(err)
  local response, encode_err = M.json({
    ok = false,
    error = err,
  }, {
    status = status_code,
    headers = opts.headers,
  })
  if response == nil then
    response = M.json({
      ok = false,
      error = vstatus.error({
        kind = "json_encode",
        message = "REST error response JSON encode failed: " ..
            tostring(encode_err and encode_err.message or encode_err),
        status = vstatus.ERR_INVALID,
        source_code = vstatus.ERROR_SOURCE_LONEJSON,
      }),
    }, {
      status = opts.fallback_status or 500,
      headers = opts.headers,
    })
    if response == nil then
      return static_error_response(opts.fallback_status or 500)
    end
  end
  return response
end

local function decode_request_json(request, opts)
  if opts.decode_json == false then
    return nil
  end
  if request.body == nil or request.body == "" then
    return nil
  end
  local ok, decoded = pcall(lonejson.decode_json, request.body)
  if ok then
    request.json = decoded
    return nil
  end
  return M.error_response({
    kind = "json_parse",
    message = tostring(decoded),
    status = vstatus.ERR_INVALID,
    source_code = vstatus.ERROR_SOURCE_LONEJSON,
  }, {status = 400})
end

local function validation_response(opts, request)
  if type(opts.validate) ~= "function" then
    return nil
  end
  local ok, valid, detail = pcall(opts.validate, request.json, request)
  if not ok then
    return M.error_response({
      kind = "validation",
      message = tostring(valid),
      status = vstatus.ERR_INVALID,
    }, {status = 400})
  end
  if valid == nil or valid == true then
    return nil
  end
  if route_response(valid) then
    return valid
  end
  return M.error_response(detail or valid or {
    kind = "validation",
    message = "request validation failed",
    status = vstatus.ERR_INVALID,
  }, {status = 400})
end

local function normalize_handler_result(result, response_opts)
  if result == nil and response_opts == nil then
    return nil
  end
  if route_response(result) then
    return result
  end
  local response, err = M.json(result, response_opts)
  if response ~= nil then
    return response
  end
  return M.error_response(err, {status = 500})
end

local function wrap_handler(opts)
  if type(opts.handler) ~= "function" then
    error("vectis.rest.route requires handler", 3)
  end
  return function(request)
    local response = decode_request_json(request, opts)
    if response ~= nil then
      return response
    end
    response = validation_response(opts, request)
    if response ~= nil then
      return response
    end
    local ok, result, response_opts = pcall(opts.handler, request)
    if not ok then
      if type(opts.error_handler) == "function" then
        local handled = opts.error_handler(result, request)
        if handled ~= nil then
          return handled
        end
      end
      return M.error_response({
        kind = "handler",
        message = tostring(result),
        status = vstatus.ERR_STATE,
      }, {status = 500})
    end
    return normalize_handler_result(result, response_opts or opts.response)
  end
end

function M.route(server, opts)
  if type(server) ~= "userdata" then
    error("vectis.rest.route requires a server", 2)
  end
  if type(opts) ~= "table" then
    error("vectis.rest.route requires options", 2)
  end
  local route = copy_server_route_options(opts)
  route.handler = wrap_handler(opts)
  if route.body == nil and opts.decode_json ~= false then
    route.body = {
      mode = "buffered",
      max_bytes = opts.max_body_bytes or 1048576,
    }
  end
  return server:route(route)
end

function M.group(server, opts)
  if type(server) ~= "userdata" then
    error("vectis.rest.group requires a server", 2)
  end
  if type(opts) ~= "table" then
    error("vectis.rest.group requires options", 2)
  end
  if type(opts.routes) ~= "table" then
    error("vectis.rest.group requires routes", 2)
  end
  local group = copy_server_route_options(opts)
  group.routes = {}
  for index, route_opts in ipairs(opts.routes) do
    if type(route_opts) ~= "table" then
      error("vectis.rest.group route must be a table", 2)
    end
    local merged = merge_defaults(opts, route_opts)
    local route = copy_server_route_options(route_opts)
    route.handler = wrap_handler(merged)
    if route.body == nil and group.body == nil and merged.decode_json ~= false then
      route.body = {
        mode = "buffered",
        max_bytes = merged.max_body_bytes or 1048576,
      }
    end
    group.routes[index] = route
  end
  return server:group(group)
end

local function join_url(base_url, path)
  if type(path) ~= "string" then
    error("vectis.rest.client request path must be a string", 3)
  end
  if path:match("^https?://") then
    return path
  end
  if base_url:sub(-1) == "/" and path:sub(1, 1) == "/" then
    return base_url .. path:sub(2)
  elseif base_url:sub(-1) ~= "/" and path:sub(1, 1) ~= "/" then
    return base_url .. "/" .. path
  end
  return base_url .. path
end

local function prepare_json_request(defaults, path, opts)
  opts = merge_defaults(defaults, opts)
  opts.url = join_url(assert(defaults.base_url, "base_url is required"), path)
  opts.base_url = nil
  if opts.json ~= nil then
    local body, err = encode_json(opts.json)
    if body == nil then
      return nil, err
    end
    opts.body = body
    opts.json = nil
  elseif type(opts.body) == "table" then
    local body, err = encode_json(opts.body)
    if body == nil then
      return nil, err
    end
    opts.body = body
  end
  if opts.body ~= nil then
    opts.headers = with_json_content_type(opts.headers)
  end
  return opts
end

local function request_error(err)
  return {
    ok = false,
    transport_ok = false,
    error = vstatus.decorate_error(err, {
      status = vstatus.ERR_INVALID,
      source_code = vstatus.ERROR_SOURCE_VECTIS,
    }),
  }
end

local function prepare_or_error(defaults, path, opts)
  local request, err = prepare_json_request(defaults, path, opts)
  if request == nil then
    return nil, request_error(err)
  end
  return request
end

function M.client(defaults)
  defaults = copy_table(defaults)
  local raw_defaults = copy_table(defaults)
  raw_defaults.base_url = nil
  local raw = http.client(raw_defaults)
  local client = {raw = raw}

  function client.request(path, opts)
    local request, err = prepare_or_error(defaults, path, opts)
    if request == nil then
      return err
    end
    return raw.request_json(request)
  end

  function client.get(path, opts)
    local err
    opts, err = prepare_or_error(defaults, path, opts)
    if opts == nil then
      return err
    end
    opts.method = opts.method or "GET"
    return raw.request_json(opts)
  end

  function client.post(path, opts)
    local err
    opts, err = prepare_or_error(defaults, path, opts)
    if opts == nil then
      return err
    end
    opts.method = opts.method or "POST"
    return raw.request_json(opts)
  end

  function client.put(path, opts)
    local err
    opts, err = prepare_or_error(defaults, path, opts)
    if opts == nil then
      return err
    end
    opts.method = opts.method or "PUT"
    return raw.request_json(opts)
  end

  function client.patch(path, opts)
    local err
    opts, err = prepare_or_error(defaults, path, opts)
    if opts == nil then
      return err
    end
    opts.method = opts.method or "PATCH"
    return raw.request_json(opts)
  end

  function client.delete(path, opts)
    local err
    opts, err = prepare_or_error(defaults, path, opts)
    if opts == nil then
      return err
    end
    opts.method = opts.method or "DELETE"
    return raw.request_json(opts)
  end

  return client
end

return M
