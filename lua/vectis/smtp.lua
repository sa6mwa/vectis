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

local function recipients(opts)
  local rcpt = opts.rcpt or opts.recipients or opts.to
  if type(rcpt) == "string" then
    return {rcpt}
  end
  return rcpt
end

function M.error_for(result)
  if type(result) ~= "table" then
    return {
      kind = "invalid_result",
      message = "SMTP result is invalid",
    }
  end
  if result.ok ~= true then
    return {
      kind = "transport",
      code = result.code,
      code_name = result.code_name,
      message = result.error or result.code_name or "curl SMTP transfer failed",
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

function M.send(first, second)
  local opts = opts_from(first, second)
  local smtp = copy_table(opts.smtp)
  local rcpt = recipients(opts)

  if opts.url == nil then
    error("vectis.smtp.send requires url", 2)
  end
  smtp.mail_from = smtp.mail_from or opts.mail_from or opts.from
  smtp.rcpt = smtp.rcpt or rcpt
  if smtp.mail_from == nil then
    error("vectis.smtp.send requires mail_from or from", 2)
  end
  if smtp.rcpt == nil then
    error("vectis.smtp.send requires rcpt, recipients, or to", 2)
  end
  if opts.body == nil and opts.message ~= nil then
    opts.body = opts.message
  end
  if opts.body == nil and opts.body_path == nil and opts.upload_path == nil then
    error("vectis.smtp.send requires body, message, body_path, or upload_path", 2)
  end
  if opts.protocols == nil then
    opts.protocols = "smtp,smtps"
  end
  opts.smtp = smtp
  return M.normalize(curl.perform(opts))
end

return M
