set(script "${WORK_DIR}/vectis-pack-smoke.lua")
set(output "${WORK_DIR}/vectis-pack-smoke")
set(bundle_script "${WORK_DIR}/vectis-pack-bundle.lua")
set(bundle "${WORK_DIR}/lockd-client.pem")
set(bundle_output "${WORK_DIR}/vectis-pack-bundle")
set(corrupt_output "${WORK_DIR}/vectis-pack-bundle-corrupt")
set(asset_dir "${WORK_DIR}/site")
set(asset_manifest "${WORK_DIR}/assets.json")
set(asset_manifest_template "${WORK_DIR}/login-template.html")
set(content_type_map "${WORK_DIR}/content-types.json")
set(asset_script "${WORK_DIR}/vectis-pack-assets.lua")
set(asset_output "${WORK_DIR}/vectis-pack-assets")
set(asset_corrupt_output "${WORK_DIR}/vectis-pack-assets-corrupt")
set(asset_duplicate_output "${WORK_DIR}/vectis-pack-assets-duplicate")
set(asset_invalid_extract_mode_output "${WORK_DIR}/vectis-pack-invalid-extract-mode")
set(no_asset_extract_dir "${WORK_DIR}/no-assets-extract")
set(asset_extract_dir "${WORK_DIR}/extracted-site")
set(asset_webdav_cache_dir "${WORK_DIR}/webdav-cache")
set(asset_webdav_credentials "${WORK_DIR}/webdav-credentials.json")

file(WRITE "${script}" "local vectis = require(\"vectis\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(vectis.has_embedded_lockd_bundle() == false)\nassert(vectis.embedded_lockd_bundle_size() == 0)\nassert(vectis.embedded.has_assets() == false)\nassert(vectis.embedded.default_extract_policy() == \"fail_exists\")\nassert(#vectis.embedded.list(\"/\") == 0)\nlocal extracted, extract_err = vectis.embedded.extract({to = [[${no_asset_extract_dir}]]})\nassert(extracted == nil)\nassert(extract_err == \"no embedded assets\")\nlocal chunks, chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(chunks == nil)\nassert(chunks_err == \"no embedded assets\")\nassert(arg[0]:match(\"vectis%-pack%-smoke$\"))\nassert(arg[1] == \"first\")\nassert(arg[2] == \"second\")\n")

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

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${script}" --output "${asset_invalid_extract_mode_output}" --extract-mode invalid
                RESULT_VARIABLE invalid_extract_mode_result
                OUTPUT_VARIABLE invalid_extract_mode_stdout
                ERROR_VARIABLE invalid_extract_mode_stderr)
if(invalid_extract_mode_result EQUAL 0)
  message(FATAL_ERROR "invalid pack extract mode unexpectedly succeeded")
endif()
if(NOT invalid_extract_mode_stderr MATCHES "--extract-mode must be")
  message(FATAL_ERROR "invalid pack extract mode failed with unexpected error: ${invalid_extract_mode_stdout}${invalid_extract_mode_stderr}")
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
file(WRITE "${asset_dir}/assets/logo.vxsite" "VX\n")
file(WRITE "${asset_manifest_template}" "<form method=\"post\"><input name=\"username\"></form>\n")
file(WRITE "${asset_manifest}" "{\"assets\":[{\"source\":\"${asset_manifest_template}\",\"path\":\"/templates/login.html\",\"content_type\":\"text/html; charset=utf-8\"}]}\n")
file(WRITE "${content_type_map}" "{\"types\":[{\"extension\":\"vxsite\",\"content_type\":\"application/x-vectis-site\"}]}\n")
file(WRITE "${asset_script}" "local vectis = require(\"vectis\")\nlocal curl = require(\"curl\")\nlocal extract_to = [[${asset_extract_dir}]]\nlocal webdav_cache = [[${asset_webdav_cache_dir}]]\nlocal webdav_credentials = [[${asset_webdav_credentials}]]\nlocal function read_file(path)\n  local fp = assert(io.open(path, \"rb\"))\n  local body = fp:read(\"*a\")\n  fp:close()\n  return body\nend\nlocal function write_file(path, body)\n  local fp = assert(io.open(path, \"wb\"))\n  fp:write(body)\n  fp:close()\nend\nlocal b64chars = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\"\nlocal function base64(data)\n  local out = {}\n  for i = 1, #data, 3 do\n    local a = data:byte(i) or 0\n    local b = data:byte(i + 1) or 0\n    local c = data:byte(i + 2) or 0\n    local n = a * 65536 + b * 256 + c\n    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)\n    out[#out + 1] = b64chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)\n    out[#out + 1] = b64chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)\n    out[#out + 1] = pad >= 2 and \"=\" or b64chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)\n    out[#out + 1] = pad >= 1 and \"=\" or b64chars:sub(n % 64 + 1, n % 64 + 1)\n  end\n  return table.concat(out)\nend\nassert(vectis.embedded.has_assets() == true)\nassert(vectis.embedded.default_extract_policy() == \"repair\")\nassert(vectis.embedded.stat(\"/index.html\").content_type == \"text/html\")\nassert(vectis.embedded.stat(\"/assets/app.css\").content_type == \"text/css\")\nassert(vectis.embedded.stat(\"/assets/app.js\").content_type == \"application/javascript\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").content_type == \"text/plain\")\nassert(vectis.embedded.stat(\"/assets/logo.vxsite\").content_type == \"application/x-vectis-site\")\nassert(vectis.embedded.stat(\"/templates/login.html\").content_type == \"text/html; charset=utf-8\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").size == 23)\nlocal app_txt_sha = vectis.embedded.stat(\"/assets/app.txt\").sha256\nassert(#app_txt_sha == 64)\nassert(app_txt_sha:match(\"^[0-9a-f]+$\"))\nassert(vectis.embedded.read(\"/index.html\"):match(\"Acme Test\"))\nassert(vectis.embedded.read(\"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(vectis.embedded.read(\"/assets/logo.vxsite\") == \"VX\\n\")\nassert(vectis.embedded.read(\"/templates/login.html\"):match(\"username\"))\nlocal chunks = {}\nfor chunk in vectis.embedded.chunks(\"/assets/app.txt\", 8) do\n  chunks[#chunks + 1] = chunk\nend\nassert(#chunks == 3)\nassert(table.concat(chunks) == \"generic embedded asset\\n\")\nlocal listed = table.concat(vectis.embedded.list(\"/\"), \"\\n\")\nassert(listed:match(\"/index%.html\"))\nassert(listed:match(\"/assets/app%.txt\"))\nassert(listed:match(\"/assets/app%.css\"))\nassert(listed:match(\"/assets/app%.js\"))\nassert(listed:match(\"/assets/logo%.vxsite\"))\nassert(listed:match(\"/templates/login%.html\"))\nlocal missing, err = vectis.embedded.read(\"/missing.txt\")\nassert(missing == nil)\nassert(err == \"embedded asset not found\")\nlocal missing_stat, missing_stat_err = vectis.embedded.stat(\"/missing.txt\")\nassert(missing_stat == nil)\nassert(missing_stat_err == \"embedded asset not found\")\nlocal missing_chunks, missing_chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(missing_chunks == nil)\nassert(missing_chunks_err == \"embedded asset not found\")\nassert(vectis.auth.store_init({credentials_path = webdav_credentials}) == true)\nassert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}).username == \"pack-user\")\nlocal webdav_key = assert(vectis.auth.webdav_key({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}))\nassert(type(webdav_key.client_id) == \"string\")\nassert(type(webdav_key.client_secret) == \"string\")\nlocal basic_auth = \"Basic \" .. base64(webdav_key.client_id .. \":\" .. webdav_key.client_secret)\nlocal server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))\nassert(server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)\nassert(server:start() == true)\nlocal function fetch_http(path)\n  local response\n  for _ = 1, 20 do\n    response = curl.perform({url = \"http://127.0.0.1:28181\" .. path, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\n    if response.ok then return response end\n    os.execute(\"sleep 0.1\")\n  end\n  return response\nend\nlocal function request_http(method, path, body, authorization)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = authorization and {Authorization = authorization} or nil, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal served_root = fetch_http(\"/\")\nassert(served_root.ok == true, served_root.error)\nassert(served_root.status == 200)\nassert(served_root.body:match(\"Acme Test\"))\nlocal served_asset = fetch_http(\"/assets/app.txt\")\nassert(served_asset.ok == true, served_asset.error)\nassert(served_asset.status == 200)\nassert(served_asset.body == \"generic embedded asset\\n\")\nassert(served_asset.headers:lower():find(\"etag:\", 1, true))\nassert(served_asset.headers:lower():find(\"cache-control: max-age=60\", 1, true))\nlocal anonymous_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"blocked\\n\")\nassert(anonymous_put.status == 401)\nlocal webdav_read = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)\nassert(webdav_read.ok == true, webdav_read.error)\nassert(webdav_read.status == 200)\nassert(webdav_read.body == \"generic embedded asset\\n\")\nlocal webdav_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"webdav mutated\\n\", basic_auth)\nassert(webdav_put.ok == true, webdav_put.error)\nassert(webdav_put.status == 201 or webdav_put.status == 204)\nlocal webdav_mutated = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)\nassert(webdav_mutated.ok == true, webdav_mutated.error)\nassert(webdav_mutated.status == 200)\nassert(webdav_mutated.body == \"webdav mutated\\n\")\nlocal embedded_after_webdav = fetch_http(\"/assets/app.txt\")\nassert(embedded_after_webdav.body == \"generic embedded asset\\n\")\nassert(server:stop() == true)\nserver:close()\nassert(vectis.embedded.extract({to = extract_to}) == true)\nassert(read_file(extract_to .. \"/index.html\"):match(\"Acme Test\"))\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/assets/app.css\"):match(\"#123456\"))\nassert(read_file(extract_to .. \"/assets/app.js\"):match(\"vectisPackAsset\"))\nassert(read_file(extract_to .. \"/assets/logo.vxsite\") == \"VX\\n\")\nassert(read_file(extract_to .. \"/templates/login.html\"):match(\"username\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"verify\"}) == true)\nlocal extracted, extract_err = vectis.embedded.extract({to = extract_to, policy = \"fail_exists\"})\nassert(extracted == nil)\nassert(extract_err:match(\"embedded asset already exists\"))\nwrite_file(extract_to .. \"/assets/app.txt\", \"mutated\\n\")\nassert(vectis.embedded.extract({to = extract_to}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nwrite_file(extract_to .. \"/assets/app.txt\", \"mutated\\n\")\nlocal verified, verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(verified == nil)\nassert(verify_err:match(\"embedded asset verification failed\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"skip-existing\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"mutated\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nwrite_file(extract_to .. \"/user-created.txt\", \"user\\n\")\nos.remove(extract_to .. \"/assets/app.txt\")\nlocal missing_verify, missing_verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(missing_verify == nil)\nassert(missing_verify_err:match(\"embedded asset is missing\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/user-created.txt\") == \"user\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"overwrite\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\n")
file(READ "${asset_script}" asset_script_body)
string(REPLACE
       "assert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}).username == \"pack-user\")"
       "local pack_user = assert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\", totp_secret = \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\", totp_label = \"Vectis:pack-user\", issuer = \"Vectis\"}))\nassert(pack_user.username == \"pack-user\")\nassert(pack_user.totp_secret == \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_key = assert(vectis.auth.webdav_key({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}))\nassert(type(webdav_key.client_id) == \"string\")\nassert(type(webdav_key.client_secret) == \"string\")\nlocal basic_auth = \"Basic \" .. base64(webdav_key.client_id .. \":\" .. webdav_key.client_secret)\nlocal server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))"
       "local server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)"
       "local webdav_registered, webdav_register_err = server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}})\nassert(webdav_registered == true, webdav_register_err and webdav_register_err.message or tostring(webdav_register_err))\nassert(server:auth_routes({path_prefix = \"/_vectis/auth\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login\", time = 59, window = 0}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local function request_http(method, path, body, authorization)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = authorization and {Authorization = authorization} or nil, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal served_root = fetch_http(\"/\")"
       "local function request_http(method, path, body, headers)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = headers, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal login_page = fetch_http(\"/_vectis/auth/login\")\nassert(login_page.ok == true, login_page.error)\nassert(login_page.status == 200)\nassert(login_page.body:find(\"action=\\\"/_vectis/auth/webdav-key\\\"\", 1, true))\nlocal password_only_login = request_http(\"POST\", \"/_vectis/auth/webdav-key\", \"username=pack-user&password=pack-password\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_only_login.status == 401)\nassert(password_only_login.body == \"login failed\\n\")\nlocal login_response = request_http(\"POST\", \"/_vectis/auth/webdav-key\", \"username=pack-user&password=pack-password&totp_code=287082\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(login_response.ok == true, login_response.error)\nassert(login_response.status == 200)\nlocal webdav_client_id = assert(login_response.body:match(\"client_id=([^\\n]+)\"))\nlocal webdav_client_secret = assert(login_response.body:match(\"client_secret=([^\\n]+)\"))\nassert(login_response.body:match('\"purpose\":\"webdav\"'))\nlocal basic_auth = \"Basic \" .. base64(webdav_client_id .. \":\" .. webdav_client_secret)\nlocal anonymous_status = request_http(\"GET\", \"/_vectis/status\")\nassert(anonymous_status.status == 401)\nassert(anonymous_status.headers:lower():find('www-authenticate: basic realm=\"pack\"', 1, true))\nlocal guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})\nassert(guarded_status.ok == true, guarded_status.error)\nassert(guarded_status.status == 200)\nassert(guarded_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nassert(guarded_status.headers:lower():find(\"cache-control: no-store\", 1, true))\nlocal served_root = fetch_http(\"/\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_read = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)"
       "local webdav_read = request_http(\"GET\", \"/dav/assets/app.txt\", nil, {Authorization = basic_auth})"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"webdav mutated\\n\", basic_auth)"
       "local webdav_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"webdav mutated\\n\", {Authorization = basic_auth})"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_mutated = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)"
       "local webdav_mutated = request_http(\"GET\", \"/dav/assets/app.txt\", nil, {Authorization = basic_auth})"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(webdav_mutated.body == \"webdav mutated\\n\")\nlocal embedded_after_webdav = fetch_http(\"/assets/app.txt\")"
       "assert(webdav_mutated.body == \"webdav mutated\\n\")\nlocal webdav_list = request_http(\"PROPFIND\", \"/dav/assets\", nil, {Authorization = basic_auth, Depth = \"1\"})\nassert(webdav_list.ok == true, webdav_list.error)\nassert(webdav_list.status == 207)\nassert(webdav_list.body:find(\"/dav/assets/app.txt\", 1, true))\nlocal webdav_mkcol = request_http(\"MKCOL\", \"/dav/user\", nil, {Authorization = basic_auth})\nassert(webdav_mkcol.ok == true, webdav_mkcol.error)\nassert(webdav_mkcol.status == 201)\nlocal webdav_copy = request_http(\"COPY\", \"/dav/assets/app.txt\", nil, {Authorization = basic_auth, Destination = \"http://127.0.0.1:28181/dav/user/copied.txt\"})\nassert(webdav_copy.ok == true, webdav_copy.error)\nassert(webdav_copy.status == 201)\nlocal webdav_copied = request_http(\"GET\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth})\nassert(webdav_copied.status == 200)\nassert(webdav_copied.body == \"webdav mutated\\n\")\nlocal webdav_move = request_http(\"MOVE\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth, Destination = \"http://127.0.0.1:28181/dav/user/moved.txt\"})\nassert(webdav_move.ok == true, webdav_move.error)\nassert(webdav_move.status == 201)\nlocal webdav_moved = request_http(\"GET\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_moved.status == 200)\nassert(webdav_moved.body == \"webdav mutated\\n\")\nlocal webdav_old_copy = request_http(\"GET\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth})\nassert(webdav_old_copy.status == 404)\nlocal webdav_delete = request_http(\"DELETE\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_delete.ok == true, webdav_delete.error)\nassert(webdav_delete.status == 204)\nlocal webdav_deleted = request_http(\"GET\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_deleted.status == 404)\nlocal embedded_after_webdav = fetch_http(\"/assets/app.txt\")"
       asset_script_body "${asset_script_body}")
file(WRITE "${asset_script}" "${asset_script_body}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_output}" --asset-dir "/:${asset_dir}" --content-type-map "${content_type_map}" --asset-manifest "${asset_manifest}" --extract-mode repair
                RESULT_VARIABLE asset_pack_result
                OUTPUT_VARIABLE asset_pack_stdout
                ERROR_VARIABLE asset_pack_stderr)
if(NOT asset_pack_result EQUAL 0)
  message(FATAL_ERROR "vectis -a pack with assets failed: ${asset_pack_stdout}${asset_pack_stderr}")
endif()

file(REMOVE_RECURSE "${asset_extract_dir}" "${asset_webdav_cache_dir}")
file(REMOVE "${asset_webdav_credentials}")
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
