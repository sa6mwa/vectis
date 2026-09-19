set(script "${WORK_DIR}/vectis-cli-app.lua")
set(packed "${WORK_DIR}/vectis-cli-app")
set(restored "${WORK_DIR}/vectis-cli-restored")

file(WRITE "${script}" [=[
local cli = require("vectis.cli").new({
  summary = "CLI fixture",
  version = "fixture-1.0",
  default_command = "deploy",
})

cli:flag({
  long = "verbose",
  short = "v",
  env = "VECTIS_CLI_VERBOSE",
  description = "Show detail",
})
cli:option({
  long = "config",
  short = "c",
  env = "VECTIS_CLI_CONFIG",
  value = "FILE",
  required = true,
  description = "Configuration file",
})
cli:option({
  long = "log-level",
  short = "l",
  env = "LOG_LEVEL",
  value = "LEVEL",
  choices = {"trace", "debug", "info", "warn", "error"},
  description = "Application log level",
})

local deploy = cli:command("deploy", {
  summary = "Deploy a fixture",
  run = function(parsed)
    io.write("command=" .. parsed.command .. "\n")
    io.write("config=" .. parsed.options.config .. "\n")
    io.write("verbose=" .. tostring(parsed.options.verbose) .. "\n")
    io.write("log_level=" .. tostring(parsed.options.log_level) .. "\n")
    io.write("count=" .. tostring(parsed.options.count) .. "\n")
    io.write("dry_run=" .. tostring(parsed.options.dry_run) .. "\n")
    io.write("tags=" .. table.concat(parsed.options.tag or {}, ",") .. "\n")
    io.write("arguments=" .. table.concat(parsed.arguments, ",") .. "\n")
  end,
})
deploy:flag({long = "dry-run", short = "d", description = "Validate only"})
deploy:option({
  long = "count",
  short = "n",
  env = "VECTIS_CLI_COUNT",
  value = "COUNT",
  type = "integer",
  default = 1,
  description = "Number of deployments",
})
deploy:option({
  long = "tag",
  env = "VECTIS_CLI_TAG",
  value = "TAG",
  repeatable = true,
  default = {"fallback"},
  description = "Deployment tag",
})

do
  local parsed = assert(cli:parse({
    "-vv", "--config=unit", "-ltrace", "deploy", "--count", "2",
    "--tag", "one", "--tag=two", "--dry-run",
    "--", "--literal",
  }))
  assert(parsed.command == "deploy")
  assert(parsed.options.verbose == true)
  assert(parsed.options.config == "unit")
  assert(parsed.options.log_level == "trace")
  assert(parsed.options.count == 2)
  assert(parsed.options.dry_run == true)
  assert(table.concat(parsed.options.tag, ",") == "one,two")
  assert(parsed.arguments[1] == "--literal")

  local default_long = assert(cli:parse({
    "-v", "--config", "unit", "-linfo", "-n1", "--tag", "unit",
    "--dry-run",
  }))
  assert(default_long.command == "deploy")
  assert(default_long.options.dry_run == true)
  local default_short = assert(cli:parse({
    "-v", "--config", "unit", "-linfo", "-n2", "--tag", "unit",
  }))
  assert(default_short.command == "deploy")
  assert(default_short.options.count == 2)

  local missing_cli = require("vectis.cli").new()
  missing_cli:option({long = "required", value = "VALUE", required = true})
  local missing, missing_err = missing_cli:parse({})
  assert(missing == nil)
  assert(missing_err:match("--required is required"))
  missing_cli:option({
    long = "required-repeatable",
    value = "VALUE",
    required = true,
    repeatable = true,
  })
  missing, missing_err = missing_cli:parse({"--required", "value"})
  assert(missing == nil)
  assert(missing_err:match("--required%-repeatable is required"))

  local command_first = require("vectis.cli").new()
  local command_first_deploy = command_first:command("deploy")
  command_first_deploy:option({long = "output", short = "o", value = "FILE"})
  local conflict_ok, conflict_error = pcall(function()
    command_first:option({long = "output", value = "FILE"})
  end)
  assert(conflict_ok == false)
  assert(conflict_error:match("global option conflicts with a command option"))
  conflict_ok, conflict_error = pcall(function()
    command_first:flag({long = "other", short = "o"})
  end)
  assert(conflict_ok == false)
  assert(conflict_error:match("global option conflicts with a command option"))

  local key_conflict = require("vectis.cli").new()
  key_conflict:option({long = "global-config", key = "config", value = "FILE"})
  local key_conflict_run = key_conflict:command("run")
  conflict_ok, conflict_error = pcall(function()
    key_conflict_run:option({long = "config", value = "FILE"})
  end)
  assert(conflict_ok == false)
  assert(conflict_error:match("command option conflicts with a global option"))

  local required_flag = require("vectis.cli").new()
  required_flag:flag({long = "confirm", required = true})
  local required_result, required_error = required_flag:parse({})
  assert(required_result == nil)
  assert(required_error:match("--confirm is required"))
  assert(required_flag:parse({"--confirm"}).options.confirm == true)

  local grouped_help = require("vectis.cli").new()
  grouped_help:flag({long = "verbose", short = "v"})
  assert(grouped_help:parse({"-vh"}).help == true)
end

cli:main(arg)
]=])

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=environment"
          "VECTIS_CLI_VERBOSE=0"
          "LOG_LEVEL=warn"
          "VECTIS_CLI_COUNT=4"
          "VECTIS_CLI_TAG=baseline"
          "${VECTIS_BIN}" -v "${script}" -v --config source -l trace deploy
          -n3 --tag alpha --tag beta --dry-run alpha
  RESULT_VARIABLE source_result
  OUTPUT_VARIABLE source_stdout
  ERROR_VARIABLE source_stderr)
if(NOT source_result EQUAL 0 OR
   NOT source_stdout MATCHES "command=deploy" OR
   NOT source_stdout MATCHES "config=source" OR
   NOT source_stdout MATCHES "verbose=true" OR
   NOT source_stdout MATCHES "log_level=trace" OR
   NOT source_stdout MATCHES "count=3" OR
   NOT source_stdout MATCHES "dry_run=true" OR
   NOT source_stdout MATCHES "tags=alpha,beta" OR
   NOT source_stdout MATCHES "arguments=alpha")
  message(FATAL_ERROR "source CLI dispatch failed: ${source_stdout}${source_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=environment"
          "VECTIS_CLI_VERBOSE=0"
          "LOG_LEVEL=warn"
          "VECTIS_CLI_COUNT=4"
          "VECTIS_CLI_TAG=baseline"
          "${VECTIS_BIN}" "${script}" deploy environment
  RESULT_VARIABLE environment_result
  OUTPUT_VARIABLE environment_stdout
  ERROR_VARIABLE environment_stderr)
if(NOT environment_result EQUAL 0 OR
   NOT environment_stdout MATCHES "config=environment" OR
   NOT environment_stdout MATCHES "verbose=false" OR
   NOT environment_stdout MATCHES "log_level=warn" OR
   NOT environment_stdout MATCHES "count=4" OR
   NOT environment_stdout MATCHES "tags=baseline" OR
   NOT environment_stdout MATCHES "arguments=environment")
  message(FATAL_ERROR "environment CLI fallback failed: ${environment_stdout}${environment_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=environment"
          "VECTIS_CLI_VERBOSE=invalid"
          "${VECTIS_BIN}" "${script}" deploy
  RESULT_VARIABLE invalid_flag_result
  OUTPUT_VARIABLE invalid_flag_stdout
  ERROR_VARIABLE invalid_flag_stderr)
if(NOT invalid_flag_result EQUAL 64 OR
   NOT invalid_flag_stderr MATCHES "environment VECTIS_CLI_VERBOSE for --verbose must be true, false, 1, 0, yes, no, on, or off")
  message(FATAL_ERROR "invalid environment flag was accepted: ${invalid_flag_stdout}${invalid_flag_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=environment"
          "LOG_LEVEL=invalid"
          "${VECTIS_BIN}" "${script}" deploy
  RESULT_VARIABLE invalid_choice_result
  OUTPUT_VARIABLE invalid_choice_stdout
  ERROR_VARIABLE invalid_choice_stderr)
if(NOT invalid_choice_result EQUAL 64 OR
   NOT invalid_choice_stderr MATCHES "environment LOG_LEVEL for --log-level must be one of trace, debug, info, warn, error")
  message(FATAL_ERROR "invalid environment choice was accepted: ${invalid_choice_stdout}${invalid_choice_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=environment"
          "VECTIS_CLI_COUNT=invalid"
          "${VECTIS_BIN}" "${script}" deploy
  RESULT_VARIABLE invalid_type_result
  OUTPUT_VARIABLE invalid_type_stdout
  ERROR_VARIABLE invalid_type_stderr)
if(NOT invalid_type_result EQUAL 64 OR
   NOT invalid_type_stderr MATCHES "environment VECTIS_CLI_COUNT for --count requires an integer")
  message(FATAL_ERROR "invalid environment type was accepted: ${invalid_type_stdout}${invalid_type_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "LOG_LEVEL=invalid"
          "${VECTIS_BIN}" "${script}" --help
  RESULT_VARIABLE environment_help_result
  OUTPUT_VARIABLE environment_help_stdout
  ERROR_VARIABLE environment_help_stderr)
if(NOT environment_help_result EQUAL 0 OR
   NOT environment_help_stdout MATCHES "environment: LOG_LEVEL" OR
   NOT environment_help_stdout MATCHES "environment: VECTIS_CLI_CONFIG")
  message(FATAL_ERROR "CLI environment help is incomplete: ${environment_help_stdout}${environment_help_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" "${script}" --config source deploy -- --vectis
  RESULT_VARIABLE source_literal_result
  OUTPUT_VARIABLE source_literal_stdout
  ERROR_VARIABLE source_literal_stderr)
if(NOT source_literal_result EQUAL 0 OR
   NOT source_literal_stdout MATCHES "arguments=--vectis")
  message(FATAL_ERROR "source script --vectis was intercepted: ${source_literal_stdout}${source_literal_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" "${script}" --help
  RESULT_VARIABLE source_help_result
  OUTPUT_VARIABLE source_help_stdout
  ERROR_VARIABLE source_help_stderr)
if(NOT source_help_result EQUAL 0 OR
   NOT source_help_stdout MATCHES "Commands:" OR
   NOT source_help_stdout MATCHES "deploy")
  message(FATAL_ERROR "source CLI help failed: ${source_help_stdout}${source_help_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" --vectis docs
  RESULT_VARIABLE generic_escape_result
  OUTPUT_VARIABLE generic_escape_stdout
  ERROR_VARIABLE generic_escape_stderr)
if(NOT generic_escape_result EQUAL 64 OR
   NOT generic_escape_stderr MATCHES "--vectis is available only from a packed application" OR
   NOT generic_escape_stderr MATCHES "-a/--action")
  message(FATAL_ERROR "generic Vectis accepted packed namespace: ${generic_escape_stdout}${generic_escape_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" --help
  RESULT_VARIABLE runtime_help_result
  OUTPUT_VARIABLE runtime_help_stdout
  ERROR_VARIABLE runtime_help_stderr)
if(NOT runtime_help_result EQUAL 0 OR
   NOT runtime_help_stdout MATCHES "Vectis runs Lua applications")
  message(FATAL_ERROR "generic Vectis help failed: ${runtime_help_stdout}${runtime_help_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a docs --help
  RESULT_VARIABLE runtime_short_action_result
  OUTPUT_VARIABLE runtime_short_action_stdout
  ERROR_VARIABLE runtime_short_action_stderr)
if(NOT runtime_short_action_result EQUAL 0 OR
   NOT runtime_short_action_stdout MATCHES "Print every documentation file")
  message(FATAL_ERROR "generic Vectis -a dispatch failed: ${runtime_short_action_stdout}${runtime_short_action_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" --action docs --help
  RESULT_VARIABLE runtime_long_action_result
  OUTPUT_VARIABLE runtime_long_action_stdout
  ERROR_VARIABLE runtime_long_action_stderr)
if(NOT runtime_long_action_result EQUAL 0 OR
   NOT runtime_long_action_stdout MATCHES "Print every documentation file")
  message(FATAL_ERROR "generic Vectis --action dispatch failed: ${runtime_long_action_stdout}${runtime_long_action_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a pack --help
  RESULT_VARIABLE pack_help_result
  OUTPUT_VARIABLE pack_help_stdout
  ERROR_VARIABLE pack_help_stderr)
if(NOT pack_help_result EQUAL 0 OR
   NOT pack_help_stdout MATCHES "-s, --script FILE" OR
   NOT pack_help_stdout MATCHES "-o, --output FILE")
  message(FATAL_ERROR "pack short-option help is incomplete: ${pack_help_stdout}${pack_help_stderr}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a pack -s "${script}" -o "${packed}"
  RESULT_VARIABLE pack_result
  OUTPUT_VARIABLE pack_stdout
  ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "CLI fixture pack failed: ${pack_stdout}${pack_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --config packed deploy --count=4 payload
  RESULT_VARIABLE packed_result
  OUTPUT_VARIABLE packed_stdout
  ERROR_VARIABLE packed_stderr)
if(NOT packed_result EQUAL 0 OR
   NOT packed_stdout MATCHES "config=packed" OR
   NOT packed_stdout MATCHES "count=4" OR
   NOT packed_stdout MATCHES "arguments=payload")
  message(FATAL_ERROR "packed CLI dispatch failed: ${packed_stdout}${packed_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "VECTIS_CLI_CONFIG=packed-environment"
          "VECTIS_CLI_VERBOSE=yes"
          "LOG_LEVEL=debug"
          "VECTIS_CLI_COUNT=5"
          "VECTIS_CLI_TAG=packed-baseline"
          "${packed}" deploy packed-environment
  RESULT_VARIABLE packed_environment_result
  OUTPUT_VARIABLE packed_environment_stdout
  ERROR_VARIABLE packed_environment_stderr)
if(NOT packed_environment_result EQUAL 0 OR
   NOT packed_environment_stdout MATCHES "config=packed-environment" OR
   NOT packed_environment_stdout MATCHES "verbose=true" OR
   NOT packed_environment_stdout MATCHES "log_level=debug" OR
   NOT packed_environment_stdout MATCHES "count=5" OR
   NOT packed_environment_stdout MATCHES "tags=packed-baseline" OR
   NOT packed_environment_stdout MATCHES "arguments=packed-environment")
  message(FATAL_ERROR "packed CLI environment fallback failed: ${packed_environment_stdout}${packed_environment_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --help
  RESULT_VARIABLE packed_help_result
  OUTPUT_VARIABLE packed_help_stdout
  ERROR_VARIABLE packed_help_stderr)
if(NOT packed_help_result EQUAL 0 OR
   NOT packed_help_stdout MATCHES "CLI fixture" OR
   packed_help_stdout MATCHES "Vectis runs Lua applications")
  message(FATAL_ERROR "packed application help was intercepted: ${packed_help_stdout}${packed_help_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis
  RESULT_VARIABLE packed_missing_action_result
  OUTPUT_VARIABLE packed_missing_action_stdout
  ERROR_VARIABLE packed_missing_action_stderr)
if(NOT packed_missing_action_result EQUAL 64 OR
   NOT packed_missing_action_stderr MATCHES "--vectis requires a Vectis action")
  message(FATAL_ERROR "packed Vectis namespace accepted no action: ${packed_missing_action_stdout}${packed_missing_action_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis --help
  RESULT_VARIABLE packed_runtime_help_result
  OUTPUT_VARIABLE packed_runtime_help_stdout
  ERROR_VARIABLE packed_runtime_help_stderr)
if(NOT packed_runtime_help_result EQUAL 0 OR
   NOT packed_runtime_help_stdout MATCHES "Vectis runs Lua applications")
  message(FATAL_ERROR "packed Vectis namespace help failed: ${packed_runtime_help_stdout}${packed_runtime_help_stderr}")
endif()

execute_process(
  COMMAND "${packed}" -a credentials --help
  RESULT_VARIABLE packed_action_result
  OUTPUT_VARIABLE packed_action_stdout
  ERROR_VARIABLE packed_action_stderr)
if(packed_action_result EQUAL 0 OR
   NOT packed_action_stderr MATCHES "unknown option -a" OR
   packed_action_stdout MATCHES "--verify AUTHORIZATION")
  message(FATAL_ERROR "packed -a escaped application dispatch: ${packed_action_stdout}${packed_action_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --config literal -- --vectis
  RESULT_VARIABLE packed_escape_result
  OUTPUT_VARIABLE packed_escape_stdout
  ERROR_VARIABLE packed_escape_stderr)
if(NOT packed_escape_result EQUAL 0 OR
   NOT packed_escape_stdout MATCHES "config=literal" OR
   NOT packed_escape_stdout MATCHES "arguments=--vectis")
  message(FATAL_ERROR "packed --vectis escape failed: ${packed_escape_stdout}${packed_escape_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis --version
  RESULT_VARIABLE runtime_version_result
  OUTPUT_VARIABLE runtime_version_stdout
  ERROR_VARIABLE runtime_version_stderr)
if(NOT runtime_version_result EQUAL 0 OR
   NOT runtime_version_stdout MATCHES "^vectis ")
  message(FATAL_ERROR "packed Vectis version failed: ${runtime_version_stdout}${runtime_version_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis docs --help
  RESULT_VARIABLE packed_direct_action_result
  OUTPUT_VARIABLE packed_direct_action_stdout
  ERROR_VARIABLE packed_direct_action_stderr)
if(NOT packed_direct_action_result EQUAL 0 OR
   NOT packed_direct_action_stdout MATCHES "Print every documentation file")
  message(FATAL_ERROR "packed Vectis direct action dispatch failed: ${packed_direct_action_stdout}${packed_direct_action_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis -a docs
  RESULT_VARIABLE packed_short_action_result
  OUTPUT_VARIABLE packed_short_action_stdout
  ERROR_VARIABLE packed_short_action_stderr)
if(NOT packed_short_action_result EQUAL 64 OR
   NOT packed_short_action_stderr MATCHES "packed --vectis actions are direct")
  message(FATAL_ERROR "packed Vectis namespace accepted -a: ${packed_short_action_stdout}${packed_short_action_stderr}")
endif()

execute_process(
  COMMAND "${packed}" --vectis --action docs
  RESULT_VARIABLE packed_long_action_result
  OUTPUT_VARIABLE packed_long_action_stdout
  ERROR_VARIABLE packed_long_action_stderr)
if(NOT packed_long_action_result EQUAL 64 OR
   NOT packed_long_action_stderr MATCHES "packed --vectis actions are direct")
  message(FATAL_ERROR "packed Vectis namespace accepted --action: ${packed_long_action_stdout}${packed_long_action_stderr}")
endif()

file(REMOVE_RECURSE "${restored}")
execute_process(
  COMMAND "${packed}" --vectis unpack --output-dir "${restored}"
  RESULT_VARIABLE unpack_result
  OUTPUT_VARIABLE unpack_stdout
  ERROR_VARIABLE unpack_stderr)
if(NOT unpack_result EQUAL 0 OR NOT EXISTS "${restored}/app.lua")
  message(FATAL_ERROR "packed Vectis unpack failed: ${unpack_stdout}${unpack_stderr}")
endif()
