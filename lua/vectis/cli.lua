local M = {}

local Cli = {}
Cli.__index = Cli

local Command = {}
Command.__index = Command

local function fail(message)
  error("vectis.cli: " .. message, 3)
end

local function copy(value)
  local result = {}
  if value then
    for key, item in pairs(value) do
      result[key] = item
    end
  end
  return result
end

local function array_copy(value)
  local result = {}
  local index
  if value then
    for index = 1, #value do
      result[index] = value[index]
    end
  end
  return result
end

local function valid_long(value)
  return type(value) == "string" and
      value:match("^[%a%d][%a%d%-]*$") ~= nil
end

local function valid_short(value)
  return type(value) == "string" and #value == 1 and
      value:match("^[%a%d]$") ~= nil
end

local function valid_environment_name(value)
  return type(value) == "string" and value ~= "" and
      value:find("[=%z]") == nil
end

local function option_key(spec)
  return spec.key or spec.long:gsub("-", "_")
end

local function option_label(spec)
  local text = ""
  if spec.short then
    text = "-" .. spec.short
  end
  if spec.long then
    if text ~= "" then
      text = text .. ", "
    end
    text = text .. "--" .. spec.long
  end
  if not spec.flag then
    text = text .. " " .. (spec.value or "VALUE")
  end
  return text
end

local function type_value(spec, value)
  local converted
  if spec.type == nil or spec.type == "string" then
    converted = value
  elseif spec.type == "integer" then
    converted = math.tointeger(value)
    if converted == nil then
      return nil, "requires an integer"
    end
  elseif spec.type == "number" then
    converted = tonumber(value)
    if converted == nil then
      return nil, "requires a number"
    end
  else
    return nil, "uses unsupported type '" .. tostring(spec.type) .. "'"
  end
  if spec.choices then
    local allowed = false
    for _, choice in ipairs(spec.choices) do
      if converted == choice then
        allowed = true
        break
      end
    end
    if not allowed then
      return nil, "must be one of " .. table.concat(spec.choices, ", ")
    end
  end
  return converted
end

local function add_option(owner, spec, flag)
  local key
  if type(spec) ~= "table" then
    fail("option definition must be a table")
  end
  if not valid_long(spec.long) then
    fail("option long name is required and must not include dashes")
  end
  if spec.short ~= nil and not valid_short(spec.short) then
    fail("option short name must be one alphanumeric character")
  end
  if spec.env ~= nil and not valid_environment_name(spec.env) then
    fail("option '" .. spec.long .. "' has an invalid environment name")
  end
  if spec.long == "help" or spec.long == "version" or
      spec.short == "h" then
    fail("option name is reserved")
  end
  if not flag and type(spec.value) ~= "string" then
    fail("option '" .. spec.long .. "' requires a value label")
  end
  if spec.repeatable and flag then
    fail("repeatable flags are not supported")
  end
  if spec.type ~= nil and spec.type ~= "string" and spec.type ~= "integer" and
      spec.type ~= "number" then
    fail("option '" .. spec.long .. "' uses an unsupported type")
  end
  key = option_key(spec)
  if type(key) ~= "string" or key == "" then
    fail("option '" .. spec.long .. "' has an invalid key")
  end
  if owner._long[spec.long] or (spec.short and owner._short[spec.short]) then
    fail("option '" .. spec.long .. "' conflicts with an existing option")
  end
  spec = copy(spec)
  spec.flag = flag
  spec.key = key
  if flag and spec.default == nil then
    spec.default = false
  end
  owner._long[spec.long] = spec
  if spec.short then
    owner._short[spec.short] = spec
  end
  owner._options[#owner._options + 1] = spec
  return owner
end

local function new_option_owner()
  return {
    _long = {},
    _short = {},
    _options = {},
  }
end

local function set_defaults(result, options)
  for _, spec in ipairs(options) do
    if spec.repeatable then
      result.options[spec.key] = array_copy(spec.default)
    elseif spec.default ~= nil then
      result.options[spec.key] = spec.default
    end
  end
end

local function store_value(result, spec, value)
  if spec.repeatable then
    local values = result.options[spec.key]
    if values == nil then
      values = {}
      result.options[spec.key] = values
    end
    values[#values + 1] = value
  else
    result.options[spec.key] = value
  end
end

local function command_by_name(cli, name)
  return cli._commands[name]
end

local function resolve_default_command(cli)
  if cli.default_command == nil then
    return nil
  end
  return command_by_name(cli, cli.default_command)
end

local function find_option(cli, command, long_name, short_name)
  local option
  if command then
    option = long_name and command._long[long_name] or command._short[short_name]
    if option then
      return option
    end
  end
  return long_name and cli._long[long_name] or cli._short[short_name]
end

local function apply_option(result, spec, raw, explicit)
  local value
  local reason
  if spec.flag then
    if raw ~= nil then
      return nil, "option --" .. spec.long .. " does not take a value"
    end
    store_value(result, spec, true)
    explicit[spec.key] = true
    return true
  end
  if raw == nil or raw == "" then
    return nil, "option --" .. spec.long .. " requires " .. spec.value
  end
  value, reason = type_value(spec, raw)
  if value == nil then
    return nil, "option --" .. spec.long .. " " .. reason
  end
  if spec.repeatable and not explicit[spec.key] then
    result.options[spec.key] = {}
  end
  store_value(result, spec, value)
  explicit[spec.key] = true
  return true
end

local function environment_flag_value(spec, raw)
  local value = raw:lower()
  if value == "1" or value == "true" or value == "yes" or value == "on" then
    return true
  end
  if value == "0" or value == "false" or value == "no" or value == "off" then
    return false
  end
  return nil, "environment " .. spec.env .. " for --" .. spec.long ..
      " must be true, false, 1, 0, yes, no, on, or off"
end

local function apply_environment(result, options, explicit)
  local raw
  local value
  local reason
  for _, spec in ipairs(options) do
    if spec.env ~= nil and not explicit[spec.key] then
      raw = os.getenv(spec.env)
      if raw ~= nil then
        if spec.flag then
          value, reason = environment_flag_value(spec, raw)
        elseif raw == "" then
          value = nil
          reason = "environment " .. spec.env .. " for --" .. spec.long ..
              " requires " .. spec.value
        else
          value, reason = type_value(spec, raw)
          if value == nil then
            reason = "environment " .. spec.env .. " for --" .. spec.long ..
                " " .. reason
          end
        end
        if value == nil then
          return nil, reason
        end
        if spec.repeatable then
          result.options[spec.key] = {value}
        else
          result.options[spec.key] = value
        end
      end
    end
  end
  return true
end

local function check_required(options, result)
  for _, spec in ipairs(options) do
    if spec.required and (result.options[spec.key] == nil or
        (spec.repeatable and #result.options[spec.key] == 0)) then
      return nil, "option --" .. spec.long .. " is required"
    end
  end
  return true
end

function M.new(opts)
  local cli
  if opts == nil then
    opts = {}
  end
  if type(opts) ~= "table" then
    fail("new expects a table")
  end
  if opts.default_command ~= nil and type(opts.default_command) ~= "string" then
    fail("default_command must be a command name")
  end
  cli = new_option_owner()
  setmetatable(cli, Cli)
  cli.summary = opts.summary
  cli.description = opts.description
  cli.version = opts.version
  cli.program = opts.program
  cli.default_command = opts.default_command
  cli._commands = {}
  cli._command_order = {}
  return cli
end

function Cli:flag(spec)
  return add_option(self, spec, true)
end

function Cli:option(spec)
  return add_option(self, spec, false)
end

function Cli:command(name, opts)
  local command
  if type(name) ~= "string" or name:match("^[%a%d][%a%d%-]*$") == nil then
    fail("command name must contain letters, digits, or hyphens")
  end
  if self._commands[name] then
    fail("command '" .. name .. "' is already defined")
  end
  opts = opts or {}
  if type(opts) ~= "table" then
    fail("command options must be a table")
  end
  command = new_option_owner()
  setmetatable(command, Command)
  command.cli = self
  command.name = name
  command.description = opts.description
  command.summary = opts.summary
  command.run = opts.run
  if command.run ~= nil and type(command.run) ~= "function" then
    fail("command '" .. name .. "' run must be a function")
  end
  self._commands[name] = command
  self._command_order[#self._command_order + 1] = command
  return command
end

function Command:flag(spec)
  if type(spec) == "table" and
      (self.cli._long[spec.long] or
          (spec.short and self.cli._short[spec.short])) then
    fail("command option conflicts with a global option")
  end
  return add_option(self, spec, true)
end

function Command:option(spec)
  if type(spec) == "table" and
      (self.cli._long[spec.long] or
          (spec.short and self.cli._short[spec.short])) then
    fail("command option conflicts with a global option")
  end
  return add_option(self, spec, false)
end

function Cli:parse(argv)
  local index
  local command
  local result
  local stop_options
  local value
  local name
  local inline
  local spec
  local short
  local offset
  local reason
  local explicit

  if argv == nil then
    argv = _G.arg or {}
  end
  if type(argv) ~= "table" then
    return nil, "arguments must be an array"
  end
  if self.default_command and not resolve_default_command(self) then
    return nil, "default command '" .. self.default_command .. "' is not defined"
  end
  result = {
    options = {},
    arguments = {},
  }
  explicit = {}
  set_defaults(result, self._options)
  index = 1
  while index <= #argv do
    value = argv[index]
    if type(value) ~= "string" then
      return nil, "argument " .. index .. " must be a string"
    end
    if not stop_options and value == "--" then
      stop_options = true
    elseif not stop_options and (value == "--help" or value == "-h") then
      result.help = true
      result.command = command and command.name or nil
      return result
    elseif not stop_options and value == "--version" and self.version ~= nil then
      result.version = true
      return result
    elseif not stop_options and value:sub(1, 2) == "--" and #value > 2 then
      name, inline = value:match("^%-%-([^=]+)=(.*)$")
      if name == nil then
        name = value:sub(3)
      end
      if command == nil then
        local default = resolve_default_command(self)
        if default and default._long[name] then
          command = default
          result.command = command.name
          set_defaults(result, command._options)
        end
      end
      spec = find_option(self, command, name, nil)
      if spec == nil then
        return nil, "unknown option --" .. name
      end
      if not spec.flag and inline == nil then
        index = index + 1
        inline = argv[index]
      end
      local ok
      ok, reason = apply_option(result, spec, inline, explicit)
      if not ok then
        return nil, reason
      end
    elseif not stop_options and value:sub(1, 1) == "-" and value ~= "-" then
      short = value:sub(2)
      offset = 1
      while offset <= #short do
        name = short:sub(offset, offset)
        if command == nil then
          local default = resolve_default_command(self)
          if default and default._short[name] then
            command = default
            result.command = command.name
            set_defaults(result, command._options)
          end
        end
        spec = find_option(self, command, nil, name)
        if spec == nil then
          return nil, "unknown option -" .. name
        end
        if spec.flag then
          local ok
          ok, reason = apply_option(result, spec, nil, explicit)
          if not ok then
            return nil, reason
          end
          offset = offset + 1
        else
          inline = short:sub(offset + 1)
          if inline == "" then
            index = index + 1
            inline = argv[index]
          end
          local ok
          ok, reason = apply_option(result, spec, inline, explicit)
          if not ok then
            return nil, reason
          end
          break
        end
      end
    elseif command == nil and command_by_name(self, value) then
      command = command_by_name(self, value)
      result.command = command.name
      set_defaults(result, command._options)
    else
      if command == nil and #self._command_order > 0 then
        command = resolve_default_command(self)
        if command == nil then
          return nil, "unknown command '" .. value .. "'"
        end
        result.command = command.name
        set_defaults(result, command._options)
      end
      result.arguments[#result.arguments + 1] = value
    end
    index = index + 1
  end
  if command == nil and self.default_command then
    command = resolve_default_command(self)
    result.command = command.name
    set_defaults(result, command._options)
  end
  local ok
  ok, reason = apply_environment(result, self._options, explicit)
  if not ok then
    return nil, reason
  end
  if command then
    ok, reason = apply_environment(result, command._options, explicit)
    if not ok then
      return nil, reason
    end
  end
  ok, reason = check_required(self._options, result)
  if not ok then
    return nil, reason
  end
  if command then
    ok, reason = check_required(command._options, result)
    if not ok then
      return nil, reason
    end
  end
  return result
end

local function append_option_lines(lines, options)
  local width = 0
  for _, spec in ipairs(options) do
    width = math.max(width, #option_label(spec))
  end
  for _, spec in ipairs(options) do
    local description = spec.description or ""
    if spec.default ~= nil and not spec.flag and not spec.repeatable then
      description = description .. (description ~= "" and " " or "") ..
          "(default: " .. tostring(spec.default) .. ")"
    end
    if spec.env ~= nil then
      description = description .. (description ~= "" and " " or "") ..
          "(environment: " .. spec.env .. ")"
    end
    lines[#lines + 1] = string.format("  %-" .. width .. "s  %s",
                                      option_label(spec), description)
  end
end

function Cli:help(command_name, program)
  local command = command_name and command_by_name(self, command_name) or nil
  local lines = {}
  program = program or self.program or "program"
  if command_name and not command then
    return nil, "unknown command '" .. command_name .. "'"
  end
  if command then
    lines[#lines + 1] = "Usage: " .. program .. " [OPTIONS] " .. command.name ..
        " [OPTIONS] [ARGUMENT ...]"
    if command.summary or command.description then
      lines[#lines + 1] = ""
      lines[#lines + 1] = command.summary or command.description
    end
  else
    lines[#lines + 1] = "Usage: " .. program .. " [OPTIONS]" ..
        (#self._command_order > 0 and " COMMAND [OPTIONS] [ARGUMENT ...]" or
            " [ARGUMENT ...]")
    if self.summary or self.description then
      lines[#lines + 1] = ""
      lines[#lines + 1] = self.summary or self.description
    end
  end
  lines[#lines + 1] = ""
  lines[#lines + 1] = "Options:"
  lines[#lines + 1] = "  -h, --help  Show this help and exit."
  if self.version ~= nil then
    lines[#lines + 1] = "  --version   Print the application version and exit."
  end
  append_option_lines(lines, self._options)
  if command then
    append_option_lines(lines, command._options)
  elseif #self._command_order > 0 then
    lines[#lines + 1] = ""
    lines[#lines + 1] = "Commands:"
    for _, item in ipairs(self._command_order) do
      lines[#lines + 1] = string.format("  %-16s %s", item.name,
                                        item.summary or item.description or "")
    end
  end
  return table.concat(lines, "\n") .. "\n"
end

function Cli:main(argv)
  local result
  local err
  local command
  local program
  argv = argv or _G.arg or {}
  program = self.program or (type(_G.arg) == "table" and _G.arg[0]) or
      "program"
  result, err = self:parse(argv)
  if result == nil then
    io.stderr:write(program .. ": " .. err .. "\n\n")
    io.stderr:write(assert(self:help(nil, program)))
    os.exit(64)
  end
  if result.help then
    io.write(assert(self:help(result.command, program)))
    return result
  end
  if result.version then
    io.write(tostring(self.version) .. "\n")
    return result
  end
  command = result.command and self._commands[result.command] or nil
  if command and command.run then
    return command.run(result)
  end
  return result
end

M.Cli = Cli
M.Command = Command

return M
