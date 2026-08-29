set(source_output "${WORK_DIR}/vectis-source-output.lua")
set(source_dir "${WORK_DIR}/vectis-source-tree")
file(REMOVE "${source_output}")
file(REMOVE_RECURSE "${source_dir}")

execute_process(COMMAND "${VECTIS_BIN}" -h
                RESULT_VARIABLE help_result
                OUTPUT_VARIABLE help_stdout
                ERROR_VARIABLE help_stderr)
if(NOT help_result EQUAL 0 OR
   NOT help_stdout MATCHES "Usage:" OR
   NOT help_stdout MATCHES "Application options:" OR
   NOT help_stdout MATCHES "Actions:" OR
   NOT help_stdout MATCHES "source[ ]+List or export embedded Lua modules" OR
   NOT help_stdout MATCHES "Action help:" OR
   help_stdout MATCHES "--action pack --script")
  message(FATAL_ERROR "vectis -h failed: ${help_stdout}${help_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" --action source --help
                RESULT_VARIABLE source_help_result
                OUTPUT_VARIABLE source_help_stdout
                ERROR_VARIABLE source_help_stderr)
if(NOT source_help_result EQUAL 0 OR
   NOT source_help_stdout MATCHES "Selectors:" OR
   NOT source_help_stdout MATCHES "--module NAME" OR
   NOT source_help_stdout MATCHES "MODULE_OR_PATH" OR
   NOT source_help_stdout MATCHES "Output:")
  message(FATAL_ERROR "vectis --action source --help failed: ${source_help_stdout}${source_help_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" --action credentials --help
                RESULT_VARIABLE credentials_help_result
                OUTPUT_VARIABLE credentials_help_stdout
                ERROR_VARIABLE credentials_help_stderr)
if(NOT credentials_help_result EQUAL 0 OR
   NOT credentials_help_stdout MATCHES "--issue" OR
   NOT credentials_help_stdout MATCHES "--verify AUTHORIZATION" OR
   NOT credentials_help_stdout MATCHES "--revoke CLIENT_ID")
  message(FATAL_ERROR "vectis --action credentials --help failed: ${credentials_help_stdout}${credentials_help_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a docs
                RESULT_VARIABLE docs_result
                OUTPUT_VARIABLE docs_stdout
                ERROR_VARIABLE docs_stderr)
if(NOT docs_result EQUAL 0)
  message(FATAL_ERROR "vectis -a docs failed: ${docs_stdout}${docs_stderr}")
endif()
if(NOT docs_stdout MATCHES "Vectis document:.*docs/lua.md" OR
   docs_stdout MATCHES "<!-- vectis docs:" OR
   NOT docs_stdout MATCHES "Self-contained documentation and Lua source")
  message(FATAL_ERROR "vectis -a docs omitted embedded documentation")
endif()

execute_process(COMMAND "${VECTIS_BIN}" --action docs --help
                RESULT_VARIABLE docs_help_result
                OUTPUT_VARIABLE docs_help_stdout
                ERROR_VARIABLE docs_help_stderr)
if(NOT docs_help_result EQUAL 0 OR
   NOT docs_help_stdout MATCHES "docs \\[-p\\|--pager\\]" OR
   NOT docs_help_stdout MATCHES "interactive terminal pager")
  message(FATAL_ERROR "vectis --action docs --help failed: ${docs_help_stdout}${docs_help_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" --action docs --pager
                RESULT_VARIABLE docs_pager_nonterminal_result
                OUTPUT_VARIABLE docs_pager_nonterminal_stdout
                ERROR_VARIABLE docs_pager_nonterminal_stderr)
if(docs_pager_nonterminal_result EQUAL 0 OR
   NOT docs_pager_nonterminal_stderr MATCHES "failed to page documentation" OR
   NOT docs_pager_nonterminal_stderr MATCHES "invalid argument" OR
   NOT docs_pager_nonterminal_stdout STREQUAL "")
  message(FATAL_ERROR "vectis --action docs --pager did not reject a non-terminal: ${docs_pager_nonterminal_stdout}${docs_pager_nonterminal_stderr}")
endif()

find_program(vectis_script_command script)
if(vectis_script_command)
  set(docs_pager_capture "${WORK_DIR}/vectis-docs-pager.txt")
  execute_process(
    COMMAND /bin/sh -c
            "(sleep 1; printf q) | \"${vectis_script_command}\" -q -e -c '\"${VECTIS_BIN}\" --action docs -p' /dev/null"
    RESULT_VARIABLE docs_pager_result
    OUTPUT_FILE "${docs_pager_capture}"
    ERROR_VARIABLE docs_pager_stderr
    TIMEOUT 15)
  file(READ "${docs_pager_capture}" docs_pager_output)
  string(ASCII 27 docs_pager_escape)
  if(NOT docs_pager_result EQUAL 0 OR
     NOT docs_pager_output MATCHES "${docs_pager_escape}\\[\\?1049h" OR
     NOT docs_pager_output MATCHES "Vectis documentation" OR
     NOT docs_pager_output MATCHES "Vectis document:" OR
     NOT docs_pager_output MATCHES "docs/api.md")
    message(FATAL_ERROR "vectis --action docs -p did not run the terminal pager: ${docs_pager_output}${docs_pager_stderr}")
  endif()
else()
  message(STATUS "SKIP: terminal-pager exercise requires the script command")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source
                RESULT_VARIABLE source_list_result
                OUTPUT_VARIABLE source_list_stdout
                ERROR_VARIABLE source_list_stderr)
if(NOT source_list_result EQUAL 0)
  message(FATAL_ERROR "vectis -a source failed: ${source_list_stdout}${source_list_stderr}")
endif()
if(NOT source_list_stdout MATCHES "vectis.lockd" OR
   NOT source_list_stdout MATCHES "lockdc")
  message(FATAL_ERROR "vectis -a source omitted public Lua modules")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                RESULT_VARIABLE source_stdout_result
                OUTPUT_VARIABLE source_stdout
                ERROR_VARIABLE source_stderr)
if(NOT source_stdout_result EQUAL 0)
  message(FATAL_ERROR "selected vectis source failed: ${source_stdout}${source_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source vectis/auth.lua
                RESULT_VARIABLE source_path_result
                OUTPUT_VARIABLE source_path_stdout
                ERROR_VARIABLE source_path_stderr)
if(NOT source_path_result EQUAL 0 OR
   NOT source_path_stdout MATCHES "-- vectis source: vectis/auth.lua" OR
   NOT source_path_stdout MATCHES "browser_flow")
  message(FATAL_ERROR "source resource-path selection failed: ${source_path_stdout}${source_path_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source vectis.auth vectis.cai vectis.dsv lockdc libmdf
                RESULT_VARIABLE source_modules_result
                OUTPUT_VARIABLE source_modules_stdout
                ERROR_VARIABLE source_modules_stderr)
if(NOT source_modules_result EQUAL 0 OR
   NOT source_modules_stdout MATCHES "-- module: vectis.auth" OR
   NOT source_modules_stdout MATCHES "-- module: vectis.cai" OR
   NOT source_modules_stdout MATCHES "-- module: vectis.dsv" OR
   NOT source_modules_stdout MATCHES "-- module: lockdc" OR
   NOT source_modules_stdout MATCHES "-- module: libmdf")
  message(FATAL_ERROR "variadic source module selection failed: ${source_modules_stdout}${source_modules_stderr}")
endif()
if(NOT source_stdout MATCHES "-- vectis source: vectis/lockd.lua" OR
   NOT source_stdout MATCHES "-- module: vectis.lockd" OR
   NOT source_stdout MATCHES "function M.open")
  message(FATAL_ERROR "selected source was not a Lua-commented transcript")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --output "${source_output}"
                RESULT_VARIABLE source_file_result
                OUTPUT_VARIABLE source_file_stdout
                ERROR_VARIABLE source_file_stderr)
if(NOT source_file_result EQUAL 0)
  message(FATAL_ERROR "source --output failed: ${source_file_stdout}${source_file_stderr}")
endif()
file(READ "${source_output}" source_file_contents)
file(READ "${VECTIS_SOURCE_DIR}/lua/vectis/lockd.lua" source_expected_contents)
if(NOT source_file_contents STREQUAL source_expected_contents)
  message(FATAL_ERROR "source --output did not preserve raw Lua source")
endif()

file(WRITE "${source_output}" "conflicting output\n")
execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --output "${source_output}"
                RESULT_VARIABLE source_conflict_result
                OUTPUT_VARIABLE source_conflict_stdout
                ERROR_VARIABLE source_conflict_stderr)
if(source_conflict_result EQUAL 0 OR
   NOT source_conflict_stderr MATCHES "output already exists.*use --force")
  message(FATAL_ERROR "source --output did not refuse an existing file")
endif()
execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --output "${source_output}" --force
                RESULT_VARIABLE source_force_result
                OUTPUT_VARIABLE source_force_stdout
                ERROR_VARIABLE source_force_stderr)
if(NOT source_force_result EQUAL 0)
  message(FATAL_ERROR "source --output --force failed: ${source_force_stdout}${source_force_stderr}")
endif()
file(READ "${source_output}" source_forced_contents)
if(NOT source_forced_contents STREQUAL source_expected_contents)
  message(FATAL_ERROR "source --output --force did not replace the file")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --module lockdc --output-dir "${source_dir}"
                RESULT_VARIABLE source_dir_result
                OUTPUT_VARIABLE source_dir_stdout
                ERROR_VARIABLE source_dir_stderr)
if(NOT source_dir_result EQUAL 0)
  message(FATAL_ERROR "source --output-dir failed: ${source_dir_stdout}${source_dir_stderr}")
endif()
file(READ "${source_dir}/vectis/lockd.lua" source_dir_lockd)
file(READ "${source_dir}/lockdc/init.lua" source_dir_lockdc)
if(NOT source_dir_lockd STREQUAL source_expected_contents OR
   NOT source_dir_lockdc MATCHES "return M")
  message(FATAL_ERROR "source --output-dir did not preserve module paths")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --output-dir "${source_dir}"
                RESULT_VARIABLE source_dir_conflict_result
                OUTPUT_VARIABLE source_dir_conflict_stdout
                ERROR_VARIABLE source_dir_conflict_stderr)
if(source_dir_conflict_result EQUAL 0 OR
   NOT source_dir_conflict_stderr MATCHES "output already exists.*use --force")
  message(FATAL_ERROR "source --output-dir did not refuse an existing file")
endif()
execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd
                        --output-dir "${source_dir}" -f
                RESULT_VARIABLE source_dir_force_result
                OUTPUT_VARIABLE source_dir_force_stdout
                ERROR_VARIABLE source_dir_force_stderr)
if(NOT source_dir_force_result EQUAL 0)
  message(FATAL_ERROR "source --output-dir --force failed: ${source_dir_force_stdout}${source_dir_force_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module does.not.exist
                RESULT_VARIABLE source_missing_result
                OUTPUT_VARIABLE source_missing_stdout
                ERROR_VARIABLE source_missing_stderr)
if(source_missing_result EQUAL 0 OR
   NOT source_missing_stderr MATCHES "matched no modules")
  message(FATAL_ERROR "unknown source module was not rejected")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a source --module vectis.lockd --force
                RESULT_VARIABLE source_force_stdout_result
                OUTPUT_VARIABLE source_force_stdout_stdout
                ERROR_VARIABLE source_force_stdout_stderr)
if(source_force_stdout_result EQUAL 0 OR
   NOT source_force_stdout_stderr MATCHES "--force requires --output")
  message(FATAL_ERROR "source --force without a filesystem output was not rejected")
endif()
