set(script "${WORK_DIR}/vectis-cli-app.lua")
set(packed "${WORK_DIR}/vectis-cli-app")
set(restored "${WORK_DIR}/vectis-cli-restored")

file(WRITE "${script}" [=[
local cli = require("vectis.cli").new({
  summary = "CLI fixture",
  version = "fixture-1.0",
  default_command = "deploy",
})

cli:flag({long = "verbose", short = "v", description = "Show detail"})
cli:option({
  long = "config",
  short = "c",
  value = "FILE",
  required = true,
  description = "Configuration file",
})

local deploy = cli:command("deploy", {
  summary = "Deploy a fixture",
  run = function(parsed)
    io.write("command=" .. parsed.command .. "\n")
    io.write("config=" .. parsed.options.config .. "\n")
    io.write("verbose=" .. tostring(parsed.options.verbose) .. "\n")
    io.write("count=" .. tostring(parsed.options.count) .. "\n")
    io.write("dry_run=" .. tostring(parsed.options.dry_run) .. "\n")
    io.write("arguments=" .. table.concat(parsed.arguments, ",") .. "\n")
  end,
})
deploy:flag({long = "dry-run", description = "Validate only"})
deploy:option({
  long = "count",
  short = "n",
  value = "COUNT",
  type = "integer",
  default = 1,
  description = "Number of deployments",
})

do
  local parsed = assert(cli:parse({
    "-vv", "--config=unit", "deploy", "--count", "2", "--dry-run",
    "--", "--literal",
  }))
  assert(parsed.command == "deploy")
  assert(parsed.options.verbose == true)
  assert(parsed.options.config == "unit")
  assert(parsed.options.count == 2)
  assert(parsed.options.dry_run == true)
  assert(parsed.arguments[1] == "--literal")
  local missing, missing_err = cli:parse({"deploy"})
  assert(missing == nil)
  assert(missing_err:match("--config is required"))
end

cli:main(arg)
]=])

execute_process(
  COMMAND "${VECTIS_BIN}" -v "${script}" --config source deploy -n3 --dry-run alpha
  RESULT_VARIABLE source_result
  OUTPUT_VARIABLE source_stdout
  ERROR_VARIABLE source_stderr)
if(NOT source_result EQUAL 0 OR
   NOT source_stdout MATCHES "command=deploy" OR
   NOT source_stdout MATCHES "config=source" OR
   NOT source_stdout MATCHES "verbose=false" OR
   NOT source_stdout MATCHES "count=3" OR
   NOT source_stdout MATCHES "dry_run=true" OR
   NOT source_stdout MATCHES "arguments=alpha")
  message(FATAL_ERROR "source CLI dispatch failed: ${source_stdout}${source_stderr}")
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
