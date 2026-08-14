local libmdf = require("libmdf")
local softline = require("softline")
local vstatus = require("vectis.status")

local M = {
  raw = {
    libmdf = libmdf,
    softline = softline,
  },
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

local function terminal_error(message, status)
  return nil, vstatus.error({
    kind = "terminal",
    message = tostring(message or "terminal helper failed"),
    status = status or vstatus.ERR_STATE,
    source_code = vstatus.ERROR_SOURCE_VECTIS,
  })
end

local function markdown_opts(opts)
  opts = copy_table(opts)
  opts.format = opts.format or "ansi"
  return opts
end

function M.markdown(markdown, opts)
  if type(markdown) ~= "string" then
    error("vectis.terminal.markdown requires a markdown string", 2)
  end
  local ok, rendered = pcall(libmdf.render, markdown, markdown_opts(opts))
  if not ok then
    return terminal_error(rendered, vstatus.ERR_INVALID)
  end
  return rendered
end

function M.markdown_stream(reader, writer, opts)
  if type(reader) ~= "function" then
    error("vectis.terminal.markdown_stream requires a reader", 2)
  end
  if type(writer) ~= "function" then
    error("vectis.terminal.markdown_stream requires a writer", 2)
  end
  local ok, rendered_or_err =
      pcall(libmdf.render_stream, reader, writer, markdown_opts(opts))
  if not ok then
    return terminal_error(rendered_or_err, vstatus.ERR_INVALID)
  end
  if rendered_or_err ~= true then
    return terminal_error("markdown stream render failed")
  end
  return true
end

function M.editor(opts)
  opts = copy_table(opts)
  opts.line_max_len = opts.line_max_len or 4096
  return softline.new(opts)
end

return M
