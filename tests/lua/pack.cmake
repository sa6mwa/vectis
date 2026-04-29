set(script "${WORK_DIR}/vectis-pack-smoke.lua")
set(output "${WORK_DIR}/vectis-pack-smoke")
set(bundle_script "${WORK_DIR}/vectis-pack-bundle.lua")
set(bundle "${WORK_DIR}/lockd-client.pem")
set(bundle_output "${WORK_DIR}/vectis-pack-bundle")
set(corrupt_output "${WORK_DIR}/vectis-pack-bundle-corrupt")

file(WRITE "${script}" "local vectis = require(\"vectis\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(vectis.has_embedded_lockd_bundle() == false)\nassert(vectis.embedded_lockd_bundle_size() == 0)\nassert(arg[0]:match(\"vectis%-pack%-smoke$\"))\nassert(arg[1] == \"first\")\nassert(arg[2] == \"second\")\n")

execute_process(COMMAND "${VECTIS_BIN}" pack --script "${script}" --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "vectis pack failed: ${pack_stdout}${pack_stderr}")
endif()

execute_process(COMMAND "${output}" first second
                RESULT_VARIABLE run_result
                OUTPUT_VARIABLE run_stdout
                ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "packed vectis failed: ${run_stdout}${run_stderr}")
endif()

file(WRITE "${bundle}" "embedded lockd client bundle bytes\n")
file(SIZE "${bundle}" bundle_size)
file(WRITE "${bundle_script}" "local vectis = require(\"vectis\")\nassert(vectis.has_embedded_lockd_bundle() == true)\nassert(vectis.embedded_lockd_bundle_size() == ${bundle_size})\nassert(arg[1] == \"bundle-arg\")\n")

execute_process(COMMAND "${VECTIS_BIN}" pack --script "${bundle_script}" --output "${bundle_output}" --lockd-bundle "${bundle}"
                RESULT_VARIABLE bundle_pack_result
                OUTPUT_VARIABLE bundle_pack_stdout
                ERROR_VARIABLE bundle_pack_stderr)
if(NOT bundle_pack_result EQUAL 0)
  message(FATAL_ERROR "vectis pack with bundle failed: ${bundle_pack_stdout}${bundle_pack_stderr}")
endif()

execute_process(COMMAND "${bundle_output}" bundle-arg
                RESULT_VARIABLE bundle_run_result
                OUTPUT_VARIABLE bundle_run_stdout
                ERROR_VARIABLE bundle_run_stderr)
if(NOT bundle_run_result EQUAL 0)
  message(FATAL_ERROR "packed vectis with bundle failed: ${bundle_run_stdout}${bundle_run_stderr}")
endif()

file(COPY_FILE "${bundle_output}" "${corrupt_output}")
file(SIZE "${corrupt_output}" corrupt_size)
math(EXPR bundle_offset "${corrupt_size} - 128 - ${bundle_size}")
execute_process(COMMAND /bin/sh -c "printf X | dd of=\"$1\" bs=1 seek=\"$2\" conv=notrunc status=none"
                "_" "${corrupt_output}" "${bundle_offset}"
                RESULT_VARIABLE corrupt_patch_result
                OUTPUT_VARIABLE corrupt_patch_stdout
                ERROR_VARIABLE corrupt_patch_stderr)
if(NOT corrupt_patch_result EQUAL 0)
  message(FATAL_ERROR "failed to corrupt packed bundle: ${corrupt_patch_stdout}${corrupt_patch_stderr}")
endif()

execute_process(COMMAND "${corrupt_output}"
                RESULT_VARIABLE corrupt_run_result
                OUTPUT_VARIABLE corrupt_run_stdout
                ERROR_VARIABLE corrupt_run_stderr)
if(corrupt_run_result EQUAL 0)
  message(FATAL_ERROR "corrupted packed bundle unexpectedly executed")
endif()
if(NOT corrupt_run_stderr MATCHES "embedded lockd bundle hash mismatch")
  message(FATAL_ERROR "corrupted packed bundle failed with unexpected error: ${corrupt_run_stdout}${corrupt_run_stderr}")
endif()
