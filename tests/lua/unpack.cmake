set(script "${WORK_DIR}/vectis-unpack-app.lua")
set(asset "${WORK_DIR}/vectis-unpack-asset.txt")
set(bundle "${WORK_DIR}/vectis-unpack-bundle.pem")
set(packed "${WORK_DIR}/vectis-unpack-packed")
set(restored "${WORK_DIR}/vectis-unpack-restored")
set(restored_default "${WORK_DIR}/vectis-unpack-default")
file(REMOVE "${script}" "${asset}" "${bundle}" "${packed}")
file(REMOVE_RECURSE "${restored}" "${restored_default}" "${WORK_DIR}/not-packed")

file(WRITE "${script}" "assert(arg[1] == 'restored')\nprint('restored app')\n")
file(WRITE "${asset}" "restored asset\n")
file(WRITE "${bundle}" "restored lockd bundle\n")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${script}"
                        --output "${packed}" --lockd-bundle "${bundle}"
                        --asset "${asset}=/site/asset.txt"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "pack for unpack test failed: ${pack_stdout}${pack_stderr}")
endif()

execute_process(COMMAND "${packed}" -a docs
                RESULT_VARIABLE packed_docs_result
                OUTPUT_VARIABLE packed_docs_stdout
                ERROR_VARIABLE packed_docs_stderr)
if(NOT packed_docs_result EQUAL 0 OR
   NOT packed_docs_stdout MATCHES "Self-contained documentation and Lua source")
  message(FATAL_ERROR "packed vectis did not expose embedded docs: ${packed_docs_stdout}${packed_docs_stderr}")
endif()

execute_process(COMMAND "${packed}" -a source --module lockdc
                RESULT_VARIABLE packed_source_result
                OUTPUT_VARIABLE packed_source_stdout
                ERROR_VARIABLE packed_source_stderr)
if(NOT packed_source_result EQUAL 0 OR
   NOT packed_source_stdout MATCHES "-- module: lockdc")
  message(FATAL_ERROR "packed vectis did not expose embedded source: ${packed_source_stdout}${packed_source_stderr}")
endif()

execute_process(COMMAND "${packed}" -a unpack --output-dir "${restored}"
                RESULT_VARIABLE unpack_result
                OUTPUT_VARIABLE unpack_stdout
                ERROR_VARIABLE unpack_stderr)
if(NOT unpack_result EQUAL 0)
  message(FATAL_ERROR "packed vectis unpack failed: ${unpack_stdout}${unpack_stderr}")
endif()
foreach(required_file vectis app.lua lockd-bundle.pem assets.json assets/site/asset.txt)
  if(NOT EXISTS "${restored}/${required_file}")
    message(FATAL_ERROR "unpack omitted ${required_file}")
  endif()
endforeach()
file(READ "${restored}/app.lua" restored_script)
file(READ "${script}" expected_script)
file(READ "${restored}/lockd-bundle.pem" restored_bundle)
file(READ "${restored}/assets/site/asset.txt" restored_asset)
if(NOT restored_script STREQUAL expected_script OR
   NOT restored_bundle STREQUAL "restored lockd bundle\n" OR
   NOT restored_asset STREQUAL "restored asset\n")
  message(FATAL_ERROR "unpack did not preserve packed payload bytes")
endif()

execute_process(COMMAND "${restored}/vectis" "${restored}/app.lua" restored
                RESULT_VARIABLE restored_result
                OUTPUT_VARIABLE restored_stdout
                ERROR_VARIABLE restored_stderr)
if(NOT restored_result EQUAL 0 OR NOT restored_stdout MATCHES "restored app")
  message(FATAL_ERROR "unpacked runner did not execute app: ${restored_stdout}${restored_stderr}")
endif()

file(MAKE_DIRECTORY "${restored_default}")
execute_process(COMMAND "${packed}" -a unpack
                WORKING_DIRECTORY "${restored_default}"
                RESULT_VARIABLE default_unpack_result
                OUTPUT_VARIABLE default_unpack_stdout
                ERROR_VARIABLE default_unpack_stderr)
if(NOT default_unpack_result EQUAL 0 OR
   NOT EXISTS "${restored_default}/vectis" OR
   NOT EXISTS "${restored_default}/assets/site/asset.txt")
  message(FATAL_ERROR "default-directory unpack failed: ${default_unpack_stdout}${default_unpack_stderr}")
endif()

execute_process(COMMAND "${packed}" -a unpack --output-dir "${restored}"
                RESULT_VARIABLE repeat_unpack_result
                OUTPUT_VARIABLE repeat_unpack_stdout
                ERROR_VARIABLE repeat_unpack_stderr)
if(repeat_unpack_result EQUAL 0 OR
   NOT repeat_unpack_stderr MATCHES "output already exists")
  message(FATAL_ERROR "unpack overwrote existing output")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a unpack --output-dir "${WORK_DIR}/not-packed"
                RESULT_VARIABLE generic_unpack_result
                OUTPUT_VARIABLE generic_unpack_stdout
                ERROR_VARIABLE generic_unpack_stderr)
if(generic_unpack_result EQUAL 0 OR
   NOT generic_unpack_stderr MATCHES "requires a packed Vectis executable")
  message(FATAL_ERROR "generic unpack was not rejected")
endif()
