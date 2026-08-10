set(script "${WORK_DIR}/vectis-pack-smoke.lua")
set(output "${WORK_DIR}/vectis-pack-smoke")
set(bundle_script "${WORK_DIR}/vectis-pack-bundle.lua")
set(bundle "${WORK_DIR}/lockd-client.pem")
set(bundle_output "${WORK_DIR}/vectis-pack-bundle")
set(corrupt_output "${WORK_DIR}/vectis-pack-bundle-corrupt")
set(asset_dir "${WORK_DIR}/site")
set(asset_script "${WORK_DIR}/vectis-pack-assets.lua")
set(asset_output "${WORK_DIR}/vectis-pack-assets")
set(asset_corrupt_output "${WORK_DIR}/vectis-pack-assets-corrupt")
set(asset_duplicate_output "${WORK_DIR}/vectis-pack-assets-duplicate")
set(no_asset_extract_dir "${WORK_DIR}/no-assets-extract")
set(asset_extract_dir "${WORK_DIR}/extracted-site")

file(WRITE "${script}" "local vectis = require(\"vectis\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(vectis.has_embedded_lockd_bundle() == false)\nassert(vectis.embedded_lockd_bundle_size() == 0)\nassert(vectis.embedded.has_assets() == false)\nassert(#vectis.embedded.list(\"/\") == 0)\nlocal extracted, extract_err = vectis.embedded.extract({to = [[${no_asset_extract_dir}]]})\nassert(extracted == nil)\nassert(extract_err == \"no embedded assets\")\nlocal chunks, chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(chunks == nil)\nassert(chunks_err == \"no embedded assets\")\nassert(arg[0]:match(\"vectis%-pack%-smoke$\"))\nassert(arg[1] == \"first\")\nassert(arg[2] == \"second\")\n")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${script}" --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "vectis -a pack failed: ${pack_stdout}${pack_stderr}")
endif()

file(REMOVE_RECURSE "${no_asset_extract_dir}")
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

execute_process(COMMAND "${VECTIS_BIN}" --action pack --script "${bundle_script}" --output "${bundle_output}" --lockd-bundle "${bundle}"
                RESULT_VARIABLE bundle_pack_result
                OUTPUT_VARIABLE bundle_pack_stdout
                ERROR_VARIABLE bundle_pack_stderr)
if(NOT bundle_pack_result EQUAL 0)
  message(FATAL_ERROR "vectis --action pack with bundle failed: ${bundle_pack_stdout}${bundle_pack_stderr}")
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
math(EXPR bundle_offset "${corrupt_size} - 256 - ${bundle_size}")
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

file(MAKE_DIRECTORY "${asset_dir}/assets")
file(WRITE "${asset_dir}/index.html" "<!doctype html><title>Acme Test</title>\n")
file(WRITE "${asset_dir}/assets/app.txt" "generic embedded asset\n")
file(WRITE "${asset_dir}/assets/app.css" "body { color: #123456; }\n")
file(WRITE "${asset_dir}/assets/app.js" "globalThis.vectisPackAsset = true;\n")
file(WRITE "${asset_script}" "local vectis = require(\"vectis\")\nlocal extract_to = [[${asset_extract_dir}]]\nlocal function read_file(path)\n  local fp = assert(io.open(path, \"rb\"))\n  local body = fp:read(\"*a\")\n  fp:close()\n  return body\nend\nlocal function write_file(path, body)\n  local fp = assert(io.open(path, \"wb\"))\n  fp:write(body)\n  fp:close()\nend\nassert(vectis.embedded.has_assets() == true)\nassert(vectis.embedded.stat(\"/index.html\").content_type == \"text/html\")\nassert(vectis.embedded.stat(\"/assets/app.css\").content_type == \"text/css\")\nassert(vectis.embedded.stat(\"/assets/app.js\").content_type == \"application/javascript\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").content_type == \"text/plain\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").size == 23)\nlocal app_txt_sha = vectis.embedded.stat(\"/assets/app.txt\").sha256\nassert(#app_txt_sha == 64)\nassert(app_txt_sha:match(\"^[0-9a-f]+$\"))\nassert(vectis.embedded.read(\"/index.html\"):match(\"Acme Test\"))\nassert(vectis.embedded.read(\"/assets/app.txt\") == \"generic embedded asset\\n\")\nlocal chunks = {}\nfor chunk in vectis.embedded.chunks(\"/assets/app.txt\", 8) do\n  chunks[#chunks + 1] = chunk\nend\nassert(#chunks == 3)\nassert(table.concat(chunks) == \"generic embedded asset\\n\")\nlocal listed = table.concat(vectis.embedded.list(\"/\"), \"\\n\")\nassert(listed:match(\"/index%.html\"))\nassert(listed:match(\"/assets/app%.txt\"))\nassert(listed:match(\"/assets/app%.css\"))\nassert(listed:match(\"/assets/app%.js\"))\nlocal missing, err = vectis.embedded.read(\"/missing.txt\")\nassert(missing == nil)\nassert(err == \"embedded asset not found\")\nlocal missing_stat, missing_stat_err = vectis.embedded.stat(\"/missing.txt\")\nassert(missing_stat == nil)\nassert(missing_stat_err == \"embedded asset not found\")\nlocal missing_chunks, missing_chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(missing_chunks == nil)\nassert(missing_chunks_err == \"embedded asset not found\")\nassert(vectis.embedded.extract({to = extract_to}) == true)\nassert(read_file(extract_to .. \"/index.html\"):match(\"Acme Test\"))\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/assets/app.css\"):match(\"#123456\"))\nassert(read_file(extract_to .. \"/assets/app.js\"):match(\"vectisPackAsset\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"verify\"}) == true)\nlocal extracted, extract_err = vectis.embedded.extract({to = extract_to})\nassert(extracted == nil)\nassert(extract_err:match(\"embedded asset already exists\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"skip_existing\"}) == true)\nwrite_file(extract_to .. \"/assets/app.txt\", \"mutated\\n\")\nlocal verified, verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(verified == nil)\nassert(verify_err:match(\"embedded asset verification failed\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"skip_existing\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"mutated\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nwrite_file(extract_to .. \"/user-created.txt\", \"user\\n\")\nos.remove(extract_to .. \"/assets/app.txt\")\nlocal missing_verify, missing_verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(missing_verify == nil)\nassert(missing_verify_err:match(\"embedded asset is missing\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/user-created.txt\") == \"user\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"overwrite\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\n")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_output}" --asset-dir "/:${asset_dir}"
                RESULT_VARIABLE asset_pack_result
                OUTPUT_VARIABLE asset_pack_stdout
                ERROR_VARIABLE asset_pack_stderr)
if(NOT asset_pack_result EQUAL 0)
  message(FATAL_ERROR "vectis -a pack with assets failed: ${asset_pack_stdout}${asset_pack_stderr}")
endif()

file(REMOVE_RECURSE "${asset_extract_dir}")
execute_process(COMMAND "${asset_output}"
                RESULT_VARIABLE asset_run_result
                OUTPUT_VARIABLE asset_run_stdout
                ERROR_VARIABLE asset_run_stderr)
if(NOT asset_run_result EQUAL 0)
  message(FATAL_ERROR "packed vectis with assets failed: ${asset_run_stdout}${asset_run_stderr}")
endif()

file(COPY_FILE "${asset_output}" "${asset_corrupt_output}")
file(SIZE "${asset_corrupt_output}" asset_corrupt_size)
math(EXPR asset_sha_offset "${asset_corrupt_size} - 256 + 144")
execute_process(COMMAND /bin/sh -c "printf X | dd of=\"$1\" bs=1 seek=\"$2\" conv=notrunc status=none"
                "_" "${asset_corrupt_output}" "${asset_sha_offset}"
                RESULT_VARIABLE asset_corrupt_patch_result
                OUTPUT_VARIABLE asset_corrupt_patch_stdout
                ERROR_VARIABLE asset_corrupt_patch_stderr)
if(NOT asset_corrupt_patch_result EQUAL 0)
  message(FATAL_ERROR "failed to corrupt packed asset hash: ${asset_corrupt_patch_stdout}${asset_corrupt_patch_stderr}")
endif()

execute_process(COMMAND "${asset_corrupt_output}"
                RESULT_VARIABLE asset_corrupt_run_result
                OUTPUT_VARIABLE asset_corrupt_run_stdout
                ERROR_VARIABLE asset_corrupt_run_stderr)
if(asset_corrupt_run_result EQUAL 0)
  message(FATAL_ERROR "corrupted packed assets unexpectedly executed")
endif()
if(NOT asset_corrupt_run_stderr MATCHES "embedded asset payload hash mismatch")
  message(FATAL_ERROR "corrupted packed assets failed with unexpected error: ${asset_corrupt_run_stdout}${asset_corrupt_run_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_duplicate_output}" --asset "${asset_dir}/index.html=/dup.txt" --asset "${asset_dir}/assets/app.txt=/dup.txt"
                RESULT_VARIABLE asset_duplicate_result
                OUTPUT_VARIABLE asset_duplicate_stdout
                ERROR_VARIABLE asset_duplicate_stderr)
if(asset_duplicate_result EQUAL 0)
  message(FATAL_ERROR "duplicate embedded asset path unexpectedly packed")
endif()
if(NOT asset_duplicate_stderr MATCHES "duplicate embedded asset path")
  message(FATAL_ERROR "duplicate embedded asset path failed with unexpected error: ${asset_duplicate_stdout}${asset_duplicate_stderr}")
endif()
