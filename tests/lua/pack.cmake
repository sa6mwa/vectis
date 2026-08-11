include("${CMAKE_CURRENT_LIST_DIR}/port_retry.cmake")

set(script "${WORK_DIR}/vectis-pack-smoke.lua")
set(output "${WORK_DIR}/vectis-pack-smoke")
set(sibling_module "${WORK_DIR}/vectis_pack_sibling.lua")
set(bundle_script "${WORK_DIR}/vectis-pack-bundle.lua")
set(bundle_generator_script "${WORK_DIR}/vectis-pack-bundle-generator.lua")
set(bundle "${WORK_DIR}/lockd-client.pem")
set(bundle_ca_cert "${WORK_DIR}/lockd-ca-cert.pem")
set(bundle_ca_key "${WORK_DIR}/lockd-ca-key.pem")
set(bundle_output "${WORK_DIR}/vectis-pack-bundle")
set(corrupt_output "${WORK_DIR}/vectis-pack-bundle-corrupt")
set(api_service_script "${WORK_DIR}/vectis-pack-api-service.lua")
set(api_service_output "${WORK_DIR}/vectis-pack-api-service")
set(api_service_credentials "${WORK_DIR}/api-service-credentials.json")
vectis_pick_test_port(api_service_port)
set(consumer_service_script "${WORK_DIR}/vectis-pack-consumer-service.lua")
set(consumer_service_output "${WORK_DIR}/vectis-pack-consumer-service")
set(consumer_service_cache_dir "${WORK_DIR}/consumer-service-cache")
vectis_pick_test_port(consumer_service_port)
set(asset_dir "${WORK_DIR}/site")
set(asset_manifest "${WORK_DIR}/assets.json")
set(asset_invalid_path_manifest "${WORK_DIR}/assets-invalid-path.json")
set(asset_symlink_manifest "${WORK_DIR}/assets-symlink.json")
set(asset_manifest_template "${WORK_DIR}/login-template.html")
set(asset_file_login_template "${WORK_DIR}/login-template-file.html")
set(content_type_map "${WORK_DIR}/content-types.json")
set(invalid_content_type_map "${WORK_DIR}/content-types-invalid.json")
set(asset_script "${WORK_DIR}/vectis-pack-assets.lua")
set(asset_output "${WORK_DIR}/vectis-pack-assets")
set(asset_invalid_path_output "${WORK_DIR}/vectis-pack-assets-invalid-path")
set(asset_invalid_manifest_path_output "${WORK_DIR}/vectis-pack-assets-invalid-manifest-path")
set(asset_invalid_content_type_map_output "${WORK_DIR}/vectis-pack-assets-invalid-content-type-map")
set(asset_symlink_reject_output "${WORK_DIR}/vectis-pack-assets-symlink-reject")
set(asset_symlink_asset_reject_output "${WORK_DIR}/vectis-pack-assets-symlink-asset-reject")
set(asset_symlink_manifest_reject_output "${WORK_DIR}/vectis-pack-assets-symlink-manifest-reject")
set(asset_corrupt_output "${WORK_DIR}/vectis-pack-assets-corrupt")
set(asset_duplicate_output "${WORK_DIR}/vectis-pack-assets-duplicate")
set(asset_invalid_extract_mode_output "${WORK_DIR}/vectis-pack-invalid-extract-mode")
set(no_asset_extract_dir "${WORK_DIR}/no-assets-extract")
set(asset_extract_dir "${WORK_DIR}/extracted-site")
set(asset_webdav_cache_dir "${WORK_DIR}/webdav-cache")
set(asset_webdav_credentials "${WORK_DIR}/webdav-credentials.json")
set(asset_https_cert_bundle "${WORK_DIR}/pack-self-signed.pem")
set(asset_smtp_mailbox "${WORK_DIR}/pack-smtp-mailbox.txt")
vectis_pick_test_port(asset_http_port)
vectis_pick_test_port(asset_https_port)
vectis_pick_test_port(asset_disk_port)

file(WRITE "${sibling_module}" "return { value = 'packed-sibling-loaded' }\n")
file(WRITE "${script}" "local vectis = require(\"vectis\")\nlocal sibling = require(\"vectis_pack_sibling\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(sibling.value == \"packed-sibling-loaded\")\nassert(vectis.has_embedded_lockd_bundle() == false)\nassert(vectis.embedded_lockd_bundle_size() == 0)\nlocal missing_source, missing_source_err = vectis.embedded_lockd_bundle_source()\nassert(missing_source == nil)\nassert(missing_source_err == \"no embedded lockd bundle\")\nlocal missing_embedded_bundle_ok, missing_embedded_bundle_err = pcall(function()\n  vectis.server.new({lockd = {endpoints = {\"https://127.0.0.1:1\"}, client_bundle = \"embedded\"}})\nend)\nassert(missing_embedded_bundle_ok == false)\nassert(tostring(missing_embedded_bundle_err):match(\"no embedded lockd bundle\"))\nassert(vectis.embedded.has_assets() == false)\nassert(vectis.embedded.default_extract_policy() == \"fail_exists\")\nassert(#vectis.embedded.list(\"/\") == 0)\nlocal extracted, extract_err = vectis.embedded.extract({to = [[${no_asset_extract_dir}]]})\nassert(extracted == nil)\nassert(extract_err == \"no embedded assets\")\nlocal chunks, chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(chunks == nil)\nassert(chunks_err == \"no embedded assets\")\nassert(arg[0]:match(\"vectis%-pack%-smoke$\"))\nassert(arg[1] == \"first\")\nassert(arg[2] == \"second\")\n")

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

file(WRITE "${bundle_generator_script}" "local vectis = require(\"vectis\")\nlocal function read_file(path)\n  local fp = assert(io.open(path, \"rb\"))\n  local body = fp:read(\"*a\")\n  fp:close()\n  return body\nend\nassert(vectis.cert.generate_bundle({common_name = \"Vectis Pack Lockd CA\", is_ca = true, output_cert_path = [[${bundle_ca_cert}]], output_key_path = [[${bundle_ca_key}]], key_bits = 2048, valid_days = 1}) == true)\nassert(vectis.cert.generate_bundle({common_name = \"lockd-client.local\", output_bundle_path = [[${bundle}]], ca_cert_path = [[${bundle_ca_cert}]], ca_key_path = [[${bundle_ca_key}]], key_bits = 2048, valid_days = 1}) == true)\nlocal fp = assert(io.open([[${bundle}]], \"ab\"))\nfp:write(read_file([[${bundle_ca_cert}]]))\nfp:close()\n")
execute_process(COMMAND "${VECTIS_BIN}" "${bundle_generator_script}"
                RESULT_VARIABLE bundle_generate_result
                OUTPUT_VARIABLE bundle_generate_stdout
                ERROR_VARIABLE bundle_generate_stderr)
if(NOT bundle_generate_result EQUAL 0)
  message(FATAL_ERROR "failed to generate lockd client bundle: ${bundle_generate_stdout}${bundle_generate_stderr}")
endif()
file(SIZE "${bundle}" bundle_size)
file(WRITE "${bundle_script}" "local vectis = require(\"vectis\")\nlocal lockdc = require(\"lockdc\")\nassert(vectis.has_embedded_lockd_bundle() == true)\nassert(vectis.embedded_lockd_bundle_size() == ${bundle_size})\nlocal source = assert(vectis.embedded_lockd_bundle_source())\nlocal first = assert(source.read(17))\nassert(#first == 17)\nlocal second = assert(source.read(11))\nassert(#second == 11)\nassert(source.reset() == true)\nlocal replay = assert(source.read(17))\nassert(replay == first)\nsource.close()\nlocal client = assert(lockdc.open({endpoints = {\"https://127.0.0.1:1\"}, client_bundle_source = assert(vectis.embedded_lockd_bundle_source()), default_namespace = \"pack\"}))\nclient:close()\nlocal server = assert(vectis.server.new({lockd = {endpoints = {\"https://127.0.0.1:1\"}, client_bundle = \"embedded\", namespace = \"pack\"}}))\nserver:close()\nassert(arg[1] == \"bundle-arg\")\n")

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

string(CONFIGURE [=[
local vectis = require("vectis")
local curl = require("curl")
local credentials = [[@api_service_credentials@]]
local port = @api_service_port@
local function base64(data)
  local chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  for i = 1, #data, 3 do
    local a = data:byte(i) or 0
    local b = data:byte(i + 1) or 0
    local c = data:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)
    out[#out + 1] = chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
    out[#out + 1] = pad >= 2 and "=" or chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)
    out[#out + 1] = pad >= 1 and "=" or chars:sub(n % 64 + 1, n % 64 + 1)
  end
  return table.concat(out)
end
assert(vectis.auth.store_init({ credentials_path = credentials }) == true)
assert(vectis.auth.user_add({
  credentials_path = credentials,
  username = "api-user",
  password = "api-password",
}).username == "api-user")
local key = assert(vectis.auth.webdav_key({
  credentials_path = credentials,
  username = "api-user",
  password = "api-password",
}))
local authorization = "Basic " .. base64(key.client_id .. ":" .. key.client_secret)
local server = assert(vectis.server.new({
  app_name = "packed-api-service",
  bind = "127.0.0.1",
  port = port,
}))
assert(server:auth_json({
  path = "/api/status",
  auth = { kind = "native", credentials_path = credentials, realm = "api", purpose = "webdav" },
  body = '{"ok":true,"service":"api"}\n',
}) == true)
assert(server:start() == true)
local function request(headers)
  local response
  for _ = 1, 20 do
    response = curl.perform({
      url = "http://127.0.0.1:" .. port .. "/api/status",
      headers = headers,
      protocols = "http",
      timeout_ms = 2000,
      connect_timeout_ms = 1000,
      no_signal = true,
    })
    if response.ok or response.status == 401 then
      return response
    end
    os.execute("sleep 0.1")
  end
  return response
end
local anonymous = request()
assert(anonymous.status == 401)
assert(anonymous.headers:lower():find('www-authenticate: basic realm="api"', 1, true))
local authenticated = request({ Authorization = authorization })
assert(authenticated.ok == true, authenticated.error)
assert(authenticated.status == 200)
assert(authenticated.body == '{"ok":true,"service":"api"}\n')
assert(authenticated.headers:lower():find("cache-control: no-store", 1, true))
assert(server:stop() == true)
server:close()
]=] api_service_script_body @ONLY)
file(WRITE "${api_service_script}" "${api_service_script_body}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${api_service_script}" --output "${api_service_output}"
                RESULT_VARIABLE api_service_pack_result
                OUTPUT_VARIABLE api_service_pack_stdout
                ERROR_VARIABLE api_service_pack_stderr)
if(NOT api_service_pack_result EQUAL 0)
  message(FATAL_ERROR "packed API service packaging failed: ${api_service_pack_stdout}${api_service_pack_stderr}")
endif()

file(REMOVE "${api_service_credentials}")
execute_process(COMMAND "${api_service_output}"
                RESULT_VARIABLE api_service_run_result
                OUTPUT_VARIABLE api_service_run_stdout
                ERROR_VARIABLE api_service_run_stderr)
if(NOT api_service_run_result EQUAL 0)
  message(FATAL_ERROR "packed API service failed: ${api_service_run_stdout}${api_service_run_stderr}")
endif()

string(CONFIGURE [=[
local vectis = require("vectis")
local cache_dir = [[@consumer_service_cache_dir@]]
assert(vectis.has_embedded_lockd_bundle() == true)
assert(vectis.embedded_lockd_bundle_size() > 0)
local server = assert(vectis.server.new({
  app_name = "packed-consumer-service",
  bind = "127.0.0.1",
  port = @consumer_service_port@,
  lockd = {
    endpoints = { "https://127.0.0.1:1" },
    client_bundle = "embedded",
    namespace = "packed-consumer",
  },
}))
local registered, err = server:consumer_service({
  queue = "packed-consumer",
  owner = "packed-consumer",
  start = false,
  on_message = function() end,
})
assert(registered == nil)
assert(err.status == vectis.ERR_INVALID)
assert(err.message:match("direct Lua callbacks are not supported"))
registered, err = server:consumer_service({
  queue = "packed-consumer",
  owner = "packed-consumer",
  start = false,
  handler = {
    kind = "webdav_marker",
    cache_dir = cache_dir,
    site_id = "packed-consumer",
    processing_path = "/processing.txt",
    done_path = "/done.txt",
  },
})
assert(registered == true, err and err.message or tostring(err))
server:close()
]=] consumer_service_script_body @ONLY)
file(WRITE "${consumer_service_script}" "${consumer_service_script_body}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${consumer_service_script}" --output "${consumer_service_output}" --lockd-bundle "${bundle}"
                RESULT_VARIABLE consumer_service_pack_result
                OUTPUT_VARIABLE consumer_service_pack_stdout
                ERROR_VARIABLE consumer_service_pack_stderr)
if(NOT consumer_service_pack_result EQUAL 0)
  message(FATAL_ERROR "packed consumer service packaging failed: ${consumer_service_pack_stdout}${consumer_service_pack_stderr}")
endif()

file(REMOVE_RECURSE "${consumer_service_cache_dir}")
execute_process(COMMAND "${consumer_service_output}"
                RESULT_VARIABLE consumer_service_run_result
                OUTPUT_VARIABLE consumer_service_run_stdout
                ERROR_VARIABLE consumer_service_run_stderr)
if(NOT consumer_service_run_result EQUAL 0)
  message(FATAL_ERROR "packed consumer service failed: ${consumer_service_run_stdout}${consumer_service_run_stderr}")
endif()

file(MAKE_DIRECTORY "${asset_dir}/assets")
file(WRITE "${asset_dir}/index.html" "<!doctype html><title>Acme Test</title>\n")
file(WRITE "${asset_dir}/assets/app.txt" "generic embedded asset\n")
file(WRITE "${asset_dir}/assets/app.css" "body { color: #123456; }\n")
file(WRITE "${asset_dir}/assets/app.js" "globalThis.vectisPackAsset = true;\n")
file(WRITE "${asset_dir}/assets/logo.vxsite" "VX\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "app.txt" "${asset_dir}/assets/app-link.txt"
                RESULT_VARIABLE asset_symlink_result
                OUTPUT_VARIABLE asset_symlink_stdout
                ERROR_VARIABLE asset_symlink_stderr)
if(NOT asset_symlink_result EQUAL 0)
  message(FATAL_ERROR "failed to create pack asset symlink: ${asset_symlink_stdout}${asset_symlink_stderr}")
endif()
file(WRITE "${asset_manifest_template}" "<!doctype html><title>{{ login_title }}</title><form id=\"embedded-login-template\" method=\"post\" action=\"{{ continue_action }}\" data-webdav-key=\"{{ webdav_key_action }}\" data-realm=\"{{ realm }}\" data-prefix=\"{{ path_prefix }}\"><input name=\"username\"></form>\n")
file(WRITE "${asset_file_login_template}" "<!doctype html><title>{{login_title}}</title><form id=\"file-login-template\" method=\"post\" action=\"{{ continue_action }}\" data-webdav-key=\"{{ webdav_key_action }}\" data-email-token=\"{{ email_token_action }}\" data-realm=\"{{ realm }}\"><input name=\"username\"></form>\n")
file(WRITE "${asset_manifest}" "{\"assets\":[{\"source\":\"${asset_manifest_template}\",\"path\":\"/templates/login.html\",\"content_type\":\"text/html; charset=utf-8\"}]}\n")
file(WRITE "${asset_invalid_path_manifest}" "{\"assets\":[{\"source\":\"${asset_manifest_template}\",\"path\":\"/../templates/login.html\",\"content_type\":\"text/html\"}]}\n")
file(WRITE "${asset_symlink_manifest}" "{\"assets\":[{\"source\":\"${asset_dir}/assets/app-link.txt\",\"path\":\"/assets/manifest-link.txt\"}]}\n")
file(WRITE "${content_type_map}" "{\"types\":[{\"extension\":\"vxsite\",\"content_type\":\"application/x-vectis-site\"}]}\n")
file(WRITE "${invalid_content_type_map}" "{\"types\":[{\"extension\":\"bad/extension\",\"content_type\":\"application/x-vectis-site\"}]}\n")
file(WRITE "${asset_script}" "local vectis = require(\"vectis\")\nlocal curl = require(\"curl\")\nlocal extract_to = [[${asset_extract_dir}]]\nlocal webdav_cache = [[${asset_webdav_cache_dir}]]\nlocal webdav_credentials = [[${asset_webdav_credentials}]]\nlocal function read_file(path)\n  local fp = assert(io.open(path, \"rb\"))\n  local body = fp:read(\"*a\")\n  fp:close()\n  return body\nend\nlocal function write_file(path, body)\n  local fp = assert(io.open(path, \"wb\"))\n  fp:write(body)\n  fp:close()\nend\nlocal b64chars = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\"\nlocal function base64(data)\n  local out = {}\n  for i = 1, #data, 3 do\n    local a = data:byte(i) or 0\n    local b = data:byte(i + 1) or 0\n    local c = data:byte(i + 2) or 0\n    local n = a * 65536 + b * 256 + c\n    local pad = (#data - i == 0) and 2 or ((#data - i == 1) and 1 or 0)\n    out[#out + 1] = b64chars:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)\n    out[#out + 1] = b64chars:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)\n    out[#out + 1] = pad >= 2 and \"=\" or b64chars:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)\n    out[#out + 1] = pad >= 1 and \"=\" or b64chars:sub(n % 64 + 1, n % 64 + 1)\n  end\n  return table.concat(out)\nend\nassert(vectis.embedded.has_assets() == true)\nassert(vectis.embedded.default_extract_policy() == \"repair\")\nassert(vectis.embedded.stat(\"/index.html\").content_type == \"text/html\")\nassert(vectis.embedded.stat(\"/assets/app.css\").content_type == \"text/css\")\nassert(vectis.embedded.stat(\"/assets/app.js\").content_type == \"application/javascript\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").content_type == \"text/plain\")\nassert(vectis.embedded.stat(\"/assets/logo.vxsite\").content_type == \"application/x-vectis-site\")\nassert(vectis.embedded.stat(\"/templates/login.html\").content_type == \"text/html; charset=utf-8\")\nassert(vectis.embedded.stat(\"/assets/app.txt\").size == 23)\nlocal app_txt_sha = vectis.embedded.stat(\"/assets/app.txt\").sha256\nassert(#app_txt_sha == 64)\nassert(app_txt_sha:match(\"^[0-9a-f]+$\"))\nassert(vectis.embedded.read(\"/index.html\"):match(\"Acme Test\"))\nassert(vectis.embedded.read(\"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(vectis.embedded.read(\"/assets/logo.vxsite\") == \"VX\\n\")\nassert(vectis.embedded.read(\"/templates/login.html\"):match(\"username\"))\nlocal chunks = {}\nfor chunk in vectis.embedded.chunks(\"/assets/app.txt\", 8) do\n  chunks[#chunks + 1] = chunk\nend\nassert(#chunks == 3)\nassert(table.concat(chunks) == \"generic embedded asset\\n\")\nlocal listed = table.concat(vectis.embedded.list(\"/\"), \"\\n\")\nassert(listed:match(\"/index%.html\"))\nassert(listed:match(\"/assets/app%.txt\"))\nassert(listed:match(\"/assets/app%.css\"))\nassert(listed:match(\"/assets/app%.js\"))\nassert(listed:match(\"/assets/logo%.vxsite\"))\nassert(listed:match(\"/templates/login%.html\"))\nlocal missing, err = vectis.embedded.read(\"/missing.txt\")\nassert(missing == nil)\nassert(err == \"embedded asset not found\")\nlocal missing_stat, missing_stat_err = vectis.embedded.stat(\"/missing.txt\")\nassert(missing_stat == nil)\nassert(missing_stat_err == \"embedded asset not found\")\nlocal missing_chunks, missing_chunks_err = vectis.embedded.chunks(\"/missing.txt\", 4)\nassert(missing_chunks == nil)\nassert(missing_chunks_err == \"embedded asset not found\")\nassert(vectis.auth.store_init({credentials_path = webdav_credentials}) == true)\nassert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}).username == \"pack-user\")\nlocal webdav_key = assert(vectis.auth.webdav_key({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}))\nassert(type(webdav_key.client_id) == \"string\")\nassert(type(webdav_key.client_secret) == \"string\")\nlocal basic_auth = \"Basic \" .. base64(webdav_key.client_id .. \":\" .. webdav_key.client_secret)\nlocal server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))\nassert(server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)\nassert(server:start() == true)\nlocal function fetch_http(path)\n  local response\n  for _ = 1, 20 do\n    response = curl.perform({url = \"http://127.0.0.1:28181\" .. path, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\n    if response.ok then return response end\n    os.execute(\"sleep 0.1\")\n  end\n  return response\nend\nlocal function request_http(method, path, body, authorization)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = authorization and {Authorization = authorization} or nil, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal served_root = fetch_http(\"/\")\nassert(served_root.ok == true, served_root.error)\nassert(served_root.status == 200)\nassert(served_root.body:match(\"Acme Test\"))\nlocal served_asset = fetch_http(\"/assets/app.txt\")\nassert(served_asset.ok == true, served_asset.error)\nassert(served_asset.status == 200)\nassert(served_asset.body == \"generic embedded asset\\n\")\nassert(served_asset.headers:lower():find(\"etag:\", 1, true))\nassert(served_asset.headers:lower():find(\"cache-control: max-age=60\", 1, true))\nlocal anonymous_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"blocked\\n\")\nassert(anonymous_put.status == 401)\nlocal webdav_read = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)\nassert(webdav_read.ok == true, webdav_read.error)\nassert(webdav_read.status == 200)\nassert(webdav_read.body == \"generic embedded asset\\n\")\nlocal webdav_put = request_http(\"PUT\", \"/dav/assets/app.txt\", \"webdav mutated\\n\", basic_auth)\nassert(webdav_put.ok == true, webdav_put.error)\nassert(webdav_put.status == 201 or webdav_put.status == 204)\nlocal webdav_mutated = request_http(\"GET\", \"/dav/assets/app.txt\", nil, basic_auth)\nassert(webdav_mutated.ok == true, webdav_mutated.error)\nassert(webdav_mutated.status == 200)\nassert(webdav_mutated.body == \"webdav mutated\\n\")\nlocal embedded_after_webdav = fetch_http(\"/assets/app.txt\")\nassert(embedded_after_webdav.body == \"generic embedded asset\\n\")\nassert(server:stop() == true)\nserver:close()\nassert(vectis.embedded.extract({to = extract_to}) == true)\nassert(read_file(extract_to .. \"/index.html\"):match(\"Acme Test\"))\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/assets/app.css\"):match(\"#123456\"))\nassert(read_file(extract_to .. \"/assets/app.js\"):match(\"vectisPackAsset\"))\nassert(read_file(extract_to .. \"/assets/logo.vxsite\") == \"VX\\n\")\nassert(read_file(extract_to .. \"/templates/login.html\"):match(\"username\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"verify\"}) == true)\nlocal extracted, extract_err = vectis.embedded.extract({to = extract_to, policy = \"fail_exists\"})\nassert(extracted == nil)\nassert(extract_err:match(\"embedded asset already exists\"))\nwrite_file(extract_to .. \"/assets/app.txt\", \"mutated\\n\")\nassert(vectis.embedded.extract({to = extract_to}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nwrite_file(extract_to .. \"/assets/app.txt\", \"mutated\\n\")\nlocal verified, verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(verified == nil)\nassert(verify_err:match(\"embedded asset verification failed\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"skip-existing\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"mutated\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nwrite_file(extract_to .. \"/user-created.txt\", \"user\\n\")\nos.remove(extract_to .. \"/assets/app.txt\")\nlocal missing_verify, missing_verify_err = vectis.embedded.extract({to = extract_to, policy = \"verify\"})\nassert(missing_verify == nil)\nassert(missing_verify_err:match(\"embedded asset is missing\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"repair\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/user-created.txt\") == \"user\\n\")\nassert(vectis.embedded.extract({to = extract_to, policy = \"overwrite\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\n")
file(READ "${asset_script}" asset_script_body)
string(REPLACE
       "assert(vectis.embedded.stat(\"/assets/app.txt\").content_type == \"text/plain\")"
       "assert(vectis.embedded.stat(\"/assets/app.txt\").content_type == \"text/plain\")\nassert(vectis.embedded.stat(\"/assets/app-link.txt\").content_type == \"text/plain\")\nassert(vectis.embedded.stat(\"/assets/manifest-link.txt\").content_type == \"text/plain\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(vectis.embedded.read(\"/assets/app.txt\") == \"generic embedded asset\\n\")"
       "assert(vectis.embedded.read(\"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(vectis.embedded.read(\"/assets/app-link.txt\") == \"generic embedded asset\\n\")\nassert(vectis.embedded.read(\"/assets/manifest-link.txt\") == \"generic embedded asset\\n\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(app_txt_sha:match(\"^[0-9a-f]+$\"))"
       "assert(app_txt_sha:match(\"^[0-9a-f]+$\"))\nassert(vectis.embedded.stat(\"/assets/app.txt\").etag == \"\\\"\" .. app_txt_sha .. \"\\\"\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(listed:match(\"/assets/app%.txt\"))"
       "assert(listed:match(\"/assets/app%.txt\"))\nassert(listed:match(\"/assets/app%-link%.txt\"))\nassert(listed:match(\"/assets/manifest%-link%.txt\"))"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")"
       "assert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/assets/app-link.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/assets/manifest-link.txt\") == \"generic embedded asset\\n\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(read_file(extract_to .. \"/templates/login.html\"):match(\"username\"))\nassert(vectis.embedded.extract({to = extract_to, policy = \"verify\"}) == true)"
       "assert(read_file(extract_to .. \"/templates/login.html\"):match(\"username\"))\nlocal disk_server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28183}))\nassert(disk_server:static_directory({path_prefix = \"/disk\", root_dir = extract_to, content_type = \"text/plain\", index_file = \"index.html\"}) == true)\nassert(disk_server:start() == true)\nlocal disk_index\nfor _ = 1, 20 do\n  disk_index = curl.perform({url = \"http://127.0.0.1:28183/disk\", protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\n  if disk_index.ok then break end\n  os.execute(\"sleep 0.1\")\nend\nassert(disk_index.ok == true, disk_index.error)\nassert(disk_index.status == 200)\nassert(disk_index.body:match(\"Acme Test\"))\nlocal disk_asset = curl.perform({url = \"http://127.0.0.1:28183/disk/assets/app.txt\", protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nassert(disk_asset.ok == true, disk_asset.error)\nassert(disk_asset.status == 200)\nassert(disk_asset.body == \"generic embedded asset\\n\")\nassert(disk_server:stop() == true)\ndisk_server:close()\nassert(vectis.embedded.extract({to = extract_to, policy = \"verify\"}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_credentials = [[${asset_webdav_credentials}]]"
       "local webdav_credentials = [[${asset_webdav_credentials}]]\nlocal file_login_template = [[${asset_file_login_template}]]\nlocal https_cert_bundle = [[${asset_https_cert_bundle}]]\nlocal smtp_url = assert(os.getenv(\"VECTIS_PACK_SMTP_URL\"))\nlocal smtp_mailbox = assert(os.getenv(\"VECTIS_PACK_SMTP_MAILBOX\"))"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}).username == \"pack-user\")"
       "local pack_user = assert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\", totp_secret = \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\", totp_label = \"Vectis:pack-user\", issuer = \"Vectis\"}))\nassert(pack_user.username == \"pack-user\")\nassert(pack_user.totp_secret == \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\")\nlocal pack_email_user = assert(vectis.auth.user_add({credentials_path = webdav_credentials, username = \"pack-email-user\", password = \"pack-email-password\"}))\nassert(pack_email_user.username == \"pack-email-user\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(pack_user.totp_secret == \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\")"
       "assert(pack_user.totp_secret == \"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\")\nlocal pack_totp = assert(vectis.auth.totp.new(\"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\"))\nlocal totp_59 = pack_totp:generate(59)\nlocal totp_360 = pack_totp:generate(360)\nassert(totp_59 == \"287082\")\nassert(#totp_360 == 6)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local webdav_key = assert(vectis.auth.webdav_key({credentials_path = webdav_credentials, username = \"pack-user\", password = \"pack-password\"}))\nassert(type(webdav_key.client_id) == \"string\")\nassert(type(webdav_key.client_secret) == \"string\")\nlocal basic_auth = \"Basic \" .. base64(webdav_key.client_id .. \":\" .. webdav_key.client_secret)\nlocal server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))"
       "local server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28181}))"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)"
       "local webdav_registered, webdav_register_err = server:webdav_embedded_site({path_prefix = \"/dav\", cache_dir = webdav_cache, site_id = \"pack\", extract_policy = \"repair\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}})\nassert(webdav_registered == true, webdav_register_err and webdav_register_err.message or tostring(webdav_register_err))\nlocal callback_webdav_provider = assert(vectis.auth.provider_callback(function(request)\n  if request.authorization == \"Bearer callback-webdav\" and request.resource == \"/assets/app.txt\" then\n    return {action = \"allow\", principal = \"callback-webdav-user\"}\n  end\n  if request.authorization == \"Bearer direct-webdav\" and request.resource == \"/assets/app.txt\" then\n    return {action = \"allow\", principal = \"direct-webdav-user\"}\n  end\n  return {action = \"required\", status_code = 401, www_authenticate = 'Bearer realm=\"callback-dav\"'}\nend))\nassert(server:webdav_embedded_site({path_prefix = \"/callback-dav\", cache_dir = webdav_cache, site_id = \"pack-callback\", extract_policy = \"repair\", auth = {provider = callback_webdav_provider, purpose = \"webdav\", allowed_modes = {\"bearer\"}}}) == true)\nassert(server:webdav_embedded_site({path_prefix = \"/direct-dav\", cache_dir = webdav_cache, site_id = \"pack-direct\", extract_policy = \"repair\", auth = callback_webdav_provider}) == true)\nassert(server:auth_routes({path_prefix = \"/_vectis/auth\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack <Login> & Auth\", login_template_embedded_path = \"/templates/login.html\", time = 59, window = 0, required_factors = {\"password\", \"totp\", \"email_token\"}, email_token_ttl_seconds = 300, email_smtp = {url = smtp_url, mail_from = \"sender@example.test\", allowed_domain = \"example.test\", timeout_ms = 5000, connect_timeout_ms = 2000}}) == true)\nassert(server:auth_routes({path_prefix = \"/_vectis/auth-file\", credentials_path = webdav_credentials, realm = \"pack-file\", login_template_path = file_login_template}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)\nassert(server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=60\"}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       "assert(server:auth_routes({path_prefix = \"/_vectis/auth-expired\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Expired\", time = 360, window = 0, require_email_token = true, email_token_ttl_seconds = 300, email_smtp = {url = smtp_url, mail_from = \"sender@example.test\", allowed_domain = \"example.test\", timeout_ms = 5000, connect_timeout_ms = 2000}}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       "assert(server:auth_routes({path_prefix = \"/_vectis/auth-local\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Local Token\", time = 59, window = 0, require_email_token = true, email_token_ttl_seconds = 300}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:auth_routes({path_prefix = \"/_vectis/auth-local\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Local Token\", time = 59, window = 0, require_email_token = true, email_token_ttl_seconds = 300}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       "assert(server:auth_routes({path_prefix = \"/_vectis/auth-local\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Local Token\", time = 59, window = 0, require_email_token = true, email_token_ttl_seconds = 300, email_token_max_attempts = 2}) == true)\nassert(server:auth_routes({path_prefix = \"/_vectis/auth-email-only\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Email Only\", time = 59, window = 0, required_factors = {\"email_token\"}, email_token_ttl_seconds = 300}) == true)\nassert(server:auth_routes({path_prefix = \"/_vectis/auth-password-email\", credentials_path = webdav_credentials, realm = \"pack\", login_title = \"Pack Login Password Email\", time = 59, window = 0, required_factors = {\"password\", \"email_token\"}, email_token_ttl_seconds = 300}) == true)\nassert(server:auth_json({path = \"/_vectis/status\", auth = {kind = \"native\", credentials_path = webdav_credentials, realm = \"pack\", purpose = \"webdav\"}, body = '{\"ok\":true,\"surface\":\"pack\"}\\n'}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local function request_http(method, path, body, authorization)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = authorization and {Authorization = authorization} or nil, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal served_root = fetch_http(\"/\")"
       "local function request_http(method, path, body, headers)\n  return curl.perform({url = \"http://127.0.0.1:28181\" .. path, method = method, body = body, headers = headers, protocols = \"http\", timeout_ms = 2000, connect_timeout_ms = 1000, no_signal = true})\nend\nlocal login_page = fetch_http(\"/_vectis/auth/login\")\nassert(login_page.ok == true, login_page.error)\nassert(login_page.status == 200)\nassert(login_page.body:find(\"action=\\\"/_vectis/auth/continue\\\"\", 1, true))\nlocal password_only_login = request_http(\"POST\", \"/_vectis/auth/continue\", \"username=pack-user&password=pack-password\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_only_login.status == 202)\nlocal pending_transaction_id = assert(password_only_login.body:match(\"pending_transaction_id=([^\\n]+)\"))\nassert(password_only_login.body:match(\"totp_required=1\"))\nlocal token_issue = request_http(\"POST\", \"/_vectis/auth/email-token\", \"username=pack-user&email=pack-user%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(token_issue.ok == true, token_issue.error)\nassert(token_issue.status == 200)\nlocal email_transaction_id = assert(token_issue.body:match(\"transaction_id=([^\\n]+)\"))\nassert(token_issue.body:match(\"token=\") == nil)\nassert(token_issue.body:match(\"expires_at=359\"))\nlocal mailbox = read_file(smtp_mailbox)\nassert(mailbox:find(\"Subject: Vectis login token\", 1, true))\nassert(mailbox:find(\"Transaction: \" .. email_transaction_id, 1, true))\nlocal email_token = assert(mailbox:match(\"Your Vectis login token is:%s*([^%s]+)\"))\nlocal wrong_email_login = request_http(\"POST\", \"/_vectis/auth/continue\", \"username=pack-user&pending_transaction_id=\" .. pending_transaction_id .. \"&totp_code=287082&email_transaction_id=\" .. email_transaction_id .. \"&email_token=wrong\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(wrong_email_login.status == 401)\nassert(wrong_email_login.body == \"login failed\\n\")\nlocal login_response = request_http(\"POST\", \"/_vectis/auth/continue\", \"username=pack-user&pending_transaction_id=\" .. pending_transaction_id .. \"&totp_code=287082&email_transaction_id=\" .. email_transaction_id .. \"&email_token=\" .. email_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(login_response.ok == true, login_response.error)\nassert(login_response.status == 200)\nlocal replay_login = request_http(\"POST\", \"/_vectis/auth/continue\", \"username=pack-user&pending_transaction_id=\" .. pending_transaction_id .. \"&totp_code=287082&email_transaction_id=\" .. email_transaction_id .. \"&email_token=\" .. email_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(replay_login.status == 401)\nassert(replay_login.body == \"login failed\\n\")\nlocal webdav_client_id = assert(login_response.body:match(\"client_id=([^\\n]+)\"))\nlocal webdav_client_secret = assert(login_response.body:match(\"client_secret=([^\\n]+)\"))\nassert(login_response.body:match('\"purpose\":\"webdav\"'))\nlocal basic_auth = \"Basic \" .. base64(webdav_client_id .. \":\" .. webdav_client_secret)\nlocal anonymous_status = request_http(\"GET\", \"/_vectis/status\")\nassert(anonymous_status.status == 401)\nassert(anonymous_status.headers:lower():find('www-authenticate: basic realm=\"pack\"', 1, true))\nlocal guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})\nassert(guarded_status.ok == true, guarded_status.error)\nassert(guarded_status.status == 200)\nassert(guarded_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nassert(guarded_status.headers:lower():find(\"cache-control: no-store\", 1, true))\nlocal served_root = fetch_http(\"/\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})"
       "local password_basic_auth = \"Basic \" .. base64(\"pack-user:pack-password\")\nlocal password_basic_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = password_basic_auth})\nassert(password_basic_status.status == 401)\nlocal password_basic_webdav = request_http(\"GET\", \"/dav/assets/app.txt\", nil, {Authorization = password_basic_auth})\nassert(password_basic_webdav.status == 401)\nlocal guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})"
       "local callback_webdav_required = request_http(\"GET\", \"/callback-dav/assets/app.txt\")\nassert(callback_webdav_required.status == 401)\nassert(callback_webdav_required.headers:lower():find('www-authenticate: bearer realm=\"callback-dav\"', 1, true))\nlocal callback_webdav_read = request_http(\"GET\", \"/callback-dav/assets/app.txt\", nil, {Authorization = \"Bearer callback-webdav\"})\nassert(callback_webdav_read.ok == true, callback_webdav_read.error)\nassert(callback_webdav_read.status == 200)\nassert(callback_webdav_read.body == \"generic embedded asset\\n\")\nlocal direct_webdav_required = request_http(\"GET\", \"/direct-dav/assets/app.txt\")\nassert(direct_webdav_required.status == 401)\nassert(direct_webdav_required.headers:lower():find('www-authenticate: bearer realm=\"callback-dav\"', 1, true))\nlocal direct_webdav_read = request_http(\"GET\", \"/direct-dav/assets/app.txt\", nil, {Authorization = \"Bearer direct-webdav\"})\nassert(direct_webdav_read.ok == true, direct_webdav_read.error)\nassert(direct_webdav_read.status == 200)\nassert(direct_webdav_read.body == \"generic embedded asset\\n\")\nlocal guarded_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(login_page.body:find(\"action=\\\"/_vectis/auth/continue\\\"\", 1, true))\nlocal password_only_login"
       "assert(login_page.body:find(\"id=\\\"embedded-login-template\\\"\", 1, true))\nassert(login_page.body:find(\"<title>Pack &lt;Login&gt; &amp; Auth</title>\", 1, true))\nassert(login_page.body:find(\"action=\\\"/_vectis/auth/continue\\\"\", 1, true))\nassert(login_page.body:find(\"data-webdav-key=\\\"/_vectis/auth/webdav-key\\\"\", 1, true))\nassert(login_page.body:find(\"data-realm=\\\"pack\\\"\", 1, true))\nassert(login_page.body:find(\"data-prefix=\\\"/_vectis/auth\\\"\", 1, true))\nassert(login_page.body:find(\"{{\", 1, true) == nil)\nlocal file_login_page = fetch_http(\"/_vectis/auth-file/login\")\nassert(file_login_page.ok == true, file_login_page.error)\nassert(file_login_page.status == 200)\nassert(file_login_page.body:find(\"id=\\\"file-login-template\\\"\", 1, true))\nassert(file_login_page.body:find(\"<title>Vectis Login</title>\", 1, true))\nassert(file_login_page.body:find(\"action=\\\"/_vectis/auth-file/continue\\\"\", 1, true))\nassert(file_login_page.body:find(\"data-webdav-key=\\\"/_vectis/auth-file/webdav-key\\\"\", 1, true))\nassert(file_login_page.body:find(\"data-email-token=\\\"/_vectis/auth-file/email-token\\\"\", 1, true))\nassert(file_login_page.body:find(\"data-realm=\\\"pack-file\\\"\", 1, true), file_login_page.body)\nassert(file_login_page.body:find(\"{{\", 1, true) == nil)\nlocal default_login_page = fetch_http(\"/_vectis/auth-local/login\")\nassert(default_login_page.ok == true, default_login_page.error)\nassert(default_login_page.status == 200)\nassert(default_login_page.body:find(\"action=\\\"/_vectis/auth-local/continue\\\"\", 1, true), default_login_page.body)\nassert(default_login_page.body:find(\"name=\\\"email_transaction_id\\\"\", 1, true), default_login_page.body)\nassert(default_login_page.body:find(\"name=\\\"email_token\\\"\", 1, true), default_login_page.body)\nassert(default_login_page.body:find(\"action=\\\"/_vectis/auth-local/email-token\\\"\", 1, true), default_login_page.body)\nassert(default_login_page.body:find(\"name=\\\"email\\\"\", 1, true), default_login_page.body)\nassert(default_login_page.body:find(\"/_vectis/auth-local/webdav-key\", 1, true) == nil, default_login_page.body)\nlocal password_only_login"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "end\nlocal login_page = fetch_http"
       "end\nlocal function assert_no_store(response)\n  local headers = response.headers:lower()\n  local detail = headers .. \"\\nBODY:\\n\" .. (response.body or \"\")\n  assert(headers:find(\"cache-control: no-store\", 1, true), detail)\n  assert(headers:find(\"pragma: no-cache\", 1, true), detail)\n  assert(headers:find(\"expires: 0\", 1, true), detail)\nend\nlocal login_page = fetch_http"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(login_page.status == 200)\nassert(login_page.body"
       "assert(login_page.status == 200)\nassert_no_store(login_page)\nassert(login_page.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(file_login_page.status == 200)\nassert(file_login_page.body"
       "assert(file_login_page.status == 200)\nassert_no_store(file_login_page)\nassert(file_login_page.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(password_only_login.status == 202)\nlocal pending_transaction_id"
       "assert(password_only_login.status == 202)\nassert_no_store(password_only_login)\nlocal password_only_login_alias = request_http(\"POST\", \"/_vectis/auth/login\", \"username=pack-user&password=pack-password\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_only_login_alias.status == 202)\nassert_no_store(password_only_login_alias)\nlocal alias_pending_transaction_id = assert(password_only_login_alias.body:match(\"pending_transaction_id=([^\\n]+)\"))\nassert(password_only_login_alias.body:match(\"totp_required=1\"))\nlocal pending_transaction_id"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(token_issue.status == 200)\nlocal email_transaction_id"
       "assert(token_issue.status == 200)\nassert_no_store(token_issue)\nlocal email_transaction_id"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local token_issue = request_http(\"POST\", \"/_vectis/auth/email-token\", \"username=pack-user&email=pack-user%40example.test\","
       "local token_issue = request_http(\"POST\", \"/_vectis/auth/email-token\", \"username=pack-user&email=pack-user%40example.test&pending_transaction_id=\" .. pending_transaction_id,"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(login_response.status == 200)\nlocal replay_login"
       "assert(login_response.status == 200, \"login_response status=\" .. tostring(login_response.status) .. \" body=\" .. tostring(login_response.body))\nassert_no_store(login_response)\nlocal replay_login"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local email_token = assert(mailbox:match(\"Your Vectis login token is:%s*([^%s]+)\"))"
       "local function token_for_transaction(mailbox_body, transaction_id)\n  for token, id in mailbox_body:gmatch(\"Your Vectis login token is:%s*([^%s]+).-Transaction:%s*([^%s]+)\") do\n    if id == transaction_id then\n      return token\n    end\n  end\n  error(\"mailbox token transaction not found: \" .. transaction_id)\nend\nlocal email_token = token_for_transaction(mailbox, email_transaction_id)\nlocal blocked_mailbox = mailbox\nlocal blocked_issue = request_http(\"POST\", \"/_vectis/auth/email-token\", \"username=pack-user&email=pack-user%40blocked.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(blocked_issue.status == 400)\nassert(blocked_issue.body:find(\"SMTP recipient is not allowed\", 1, true))\nassert(read_file(smtp_mailbox) == blocked_mailbox)\nlocal expired_issue = request_http(\"POST\", \"/_vectis/auth-local/email-token\", \"username=pack-user&email=pack-user%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(expired_issue.ok == true, expired_issue.error)\nassert(expired_issue.status == 200)\nlocal expired_transaction_id = assert(expired_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal expired_token = assert(expired_issue.body:match(\"token=([^\\n]+)\"))\nassert(expired_issue.body:match(\"expires_at=359\"))\nlocal expired_login = request_http(\"POST\", \"/_vectis/auth-expired/continue\", \"username=pack-user&password=pack-password&totp_code=\" .. totp_360 .. \"&email_transaction_id=\" .. expired_transaction_id .. \"&email_token=\" .. expired_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(expired_login.status == 401)\nassert(expired_login.body == \"login failed\\n\")\nlocal expired_replay = request_http(\"POST\", \"/_vectis/auth-expired/continue\", \"username=pack-user&password=pack-password&totp_code=\" .. totp_360 .. \"&email_transaction_id=\" .. expired_transaction_id .. \"&email_token=\" .. expired_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(expired_replay.status == 401)\nassert(expired_replay.body == \"login failed\\n\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(wrong_email_login.status == 401)\nassert(wrong_email_login.body"
       "assert(wrong_email_login.status == 401)\nassert_no_store(wrong_email_login)\nassert(wrong_email_login.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local wrong_email_login = request_http(\"POST\", \"/_vectis/auth/continue\","
       "local wrong_pending_login = request_http(\"POST\", \"/_vectis/auth/continue\", \"username=pack-user&pending_transaction_id=\" .. alias_pending_transaction_id .. \"&totp_code=\" .. totp_59 .. \"&email_transaction_id=\" .. email_transaction_id .. \"&email_token=\" .. email_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(wrong_pending_login.status == 401)\nassert_no_store(wrong_pending_login)\nassert(wrong_pending_login.body == \"login failed\\n\")\nlocal wrong_email_login = request_http(\"POST\", \"/_vectis/auth/continue\","
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(replay_login.status == 401)\nassert(replay_login.body"
       "assert(replay_login.status == 401)\nassert_no_store(replay_login)\nassert(replay_login.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(blocked_issue.status == 400)\nassert(blocked_issue.body"
       "assert(blocked_issue.status == 400)\nassert_no_store(blocked_issue)\nassert(blocked_issue.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(expired_issue.status == 200)\nlocal expired_transaction_id"
       "assert(expired_issue.status == 200)\nassert_no_store(expired_issue)\nlocal expired_transaction_id"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(expired_login.status == 401)\nassert(expired_login.body"
       "assert(expired_login.status == 401)\nassert_no_store(expired_login)\nassert(expired_login.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(expired_replay.status == 401)\nassert(expired_replay.body"
       "assert(expired_replay.status == 401)\nassert_no_store(expired_replay)\nassert(expired_replay.body"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(expired_replay.body == \"login failed\\n\")\nlocal wrong_email_login"
       "assert(expired_replay.body == \"login failed\\n\")\nlocal limited_issue = request_http(\"POST\", \"/_vectis/auth-local/email-token\", \"username=pack-user&email=pack-user%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(limited_issue.ok == true, limited_issue.error)\nassert(limited_issue.status == 200)\nassert_no_store(limited_issue)\nlocal limited_transaction_id = assert(limited_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal limited_token = assert(limited_issue.body:match(\"token=([^\\n]+)\"))\nlocal limited_wrong_one = request_http(\"POST\", \"/_vectis/auth-local/continue\", \"username=pack-user&password=pack-password&totp_code=\" .. totp_59 .. \"&email_transaction_id=\" .. limited_transaction_id .. \"&email_token=wrong-one\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(limited_wrong_one.status == 401)\nassert_no_store(limited_wrong_one)\nlocal limited_wrong_two = request_http(\"POST\", \"/_vectis/auth-local/continue\", \"username=pack-user&password=pack-password&totp_code=\" .. totp_59 .. \"&email_transaction_id=\" .. limited_transaction_id .. \"&email_token=wrong-two\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(limited_wrong_two.status == 401)\nassert_no_store(limited_wrong_two)\nlocal limited_after_budget = request_http(\"POST\", \"/_vectis/auth-local/continue\", \"username=pack-user&password=pack-password&totp_code=\" .. totp_59 .. \"&email_transaction_id=\" .. limited_transaction_id .. \"&email_token=\" .. limited_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(limited_after_budget.status == 401)\nassert_no_store(limited_after_budget)\nassert(limited_after_budget.body == \"login failed\\n\")\nlocal email_only_unknown_issue = request_http(\"POST\", \"/_vectis/auth-email-only/email-token\", \"username=unknown-user&email=unknown%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_unknown_issue.status == 401)\nassert_no_store(email_only_unknown_issue)\nassert(email_only_unknown_issue.body == \"login failed\\n\")\nassert(email_only_unknown_issue.body:match(\"transaction_id=\") == nil)\nlocal email_only_issue = request_http(\"POST\", \"/_vectis/auth-email-only/email-token\", \"username=pack-user&email=pack-user%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_issue.ok == true, email_only_issue.error)\nassert(email_only_issue.status == 200)\nassert_no_store(email_only_issue)\nlocal email_only_transaction_id = assert(email_only_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal email_only_token = assert(email_only_issue.body:match(\"token=([^\\n]+)\"))\nlocal email_only_missing_user = request_http(\"POST\", \"/_vectis/auth-email-only/continue\", \"email_transaction_id=\" .. email_only_transaction_id .. \"&email_token=\" .. email_only_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_missing_user.status == 400)\nassert_no_store(email_only_missing_user)\nassert(email_only_missing_user.body == \"username is required\\n\")\nlocal email_only_login = request_http(\"POST\", \"/_vectis/auth-email-only/continue\", \"username=pack-user&email_transaction_id=\" .. email_only_transaction_id .. \"&email_token=\" .. email_only_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_login.ok == true, email_only_login.error)\nassert(email_only_login.status == 200)\nassert_no_store(email_only_login)\nlocal email_only_client_id = assert(email_only_login.body:match(\"client_id=([^\\n]+)\"))\nlocal email_only_client_secret = assert(email_only_login.body:match(\"client_secret=([^\\n]+)\"))\nlocal email_only_basic_auth = \"Basic \" .. base64(email_only_client_id .. \":\" .. email_only_client_secret)\nlocal email_only_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = email_only_basic_auth})\nassert(email_only_status.ok == true, email_only_status.error)\nassert(email_only_status.status == 200)\nassert(email_only_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nlocal password_email_start = request_http(\"POST\", \"/_vectis/auth-password-email/continue\", \"username=pack-email-user&password=pack-email-password\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_start.status == 202)\nassert_no_store(password_email_start)\nassert(password_email_start.body:match(\"totp_required=0\"))\nlocal password_email_pending = assert(password_email_start.body:match(\"pending_transaction_id=([^\\n]+)\"))\nlocal password_email_issue = request_http(\"POST\", \"/_vectis/auth-password-email/email-token\", \"username=pack-email-user&email=pack-email-user%40example.test&pending_transaction_id=\" .. password_email_pending, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_issue.ok == true, password_email_issue.error)\nassert(password_email_issue.status == 200)\nassert_no_store(password_email_issue)\nlocal password_email_transaction_id = assert(password_email_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal password_email_token = assert(password_email_issue.body:match(\"token=([^\\n]+)\"))\nlocal password_email_login = request_http(\"POST\", \"/_vectis/auth-password-email/continue\", \"username=pack-email-user&pending_transaction_id=\" .. password_email_pending .. \"&email_transaction_id=\" .. password_email_transaction_id .. \"&email_token=\" .. password_email_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_login.ok == true, password_email_login.error)\nassert(password_email_login.status == 200)\nassert_no_store(password_email_login)\nlocal password_email_client_id = assert(password_email_login.body:match(\"client_id=([^\\n]+)\"))\nlocal password_email_client_secret = assert(password_email_login.body:match(\"client_secret=([^\\n]+)\"))\nlocal password_email_basic_auth = \"Basic \" .. base64(password_email_client_id .. \":\" .. password_email_client_secret)\nlocal password_email_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = password_email_basic_auth})\nassert(password_email_status.ok == true, password_email_status.error)\nassert(password_email_status.status == 200)\nassert(password_email_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nlocal wrong_email_login"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local password_email_start = request_http(\"POST\", \"/_vectis/auth-password-email/continue\""
       "local email_only_webdav_key_issue = request_http(\"POST\", \"/_vectis/auth-email-only/email-token\", \"username=pack-user&email=pack-user%40example.test\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_webdav_key_issue.ok == true, email_only_webdav_key_issue.error)\nassert(email_only_webdav_key_issue.status == 200)\nassert_no_store(email_only_webdav_key_issue)\nlocal email_only_webdav_key_transaction_id = assert(email_only_webdav_key_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal email_only_webdav_key_token = assert(email_only_webdav_key_issue.body:match(\"token=([^\\n]+)\"))\nlocal email_only_webdav_key_response = request_http(\"POST\", \"/_vectis/auth-email-only/webdav-key\", \"username=pack-user&email_transaction_id=\" .. email_only_webdav_key_transaction_id .. \"&email_token=\" .. email_only_webdav_key_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(email_only_webdav_key_response.ok == true, email_only_webdav_key_response.error)\nassert(email_only_webdav_key_response.status == 200)\nassert_no_store(email_only_webdav_key_response)\nlocal email_only_webdav_key_client_id = assert(email_only_webdav_key_response.body:match(\"client_id=([^\\n]+)\"))\nlocal email_only_webdav_key_client_secret = assert(email_only_webdav_key_response.body:match(\"client_secret=([^\\n]+)\"))\nlocal email_only_webdav_key_basic_auth = \"Basic \" .. base64(email_only_webdav_key_client_id .. \":\" .. email_only_webdav_key_client_secret)\nlocal email_only_webdav_key_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = email_only_webdav_key_basic_auth})\nassert(email_only_webdav_key_status.ok == true, email_only_webdav_key_status.error)\nassert(email_only_webdav_key_status.status == 200)\nassert(email_only_webdav_key_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nlocal password_email_start = request_http(\"POST\", \"/_vectis/auth-password-email/continue\""
       asset_script_body "${asset_script_body}")
string(REPLACE
       "local wrong_email_login = request_http(\"POST\", \"/_vectis/auth/continue\","
       "local password_email_webdav_start = request_http(\"POST\", \"/_vectis/auth-password-email/continue\", \"username=pack-email-user&password=pack-email-password\", {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_webdav_start.status == 202)\nassert_no_store(password_email_webdav_start)\nlocal password_email_webdav_pending = assert(password_email_webdav_start.body:match(\"pending_transaction_id=([^\\n]+)\"))\nlocal password_email_webdav_issue = request_http(\"POST\", \"/_vectis/auth-password-email/email-token\", \"username=pack-email-user&email=pack-email-user%40example.test&pending_transaction_id=\" .. password_email_webdav_pending, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_webdav_issue.ok == true, password_email_webdav_issue.error)\nassert(password_email_webdav_issue.status == 200)\nassert_no_store(password_email_webdav_issue)\nlocal password_email_webdav_transaction_id = assert(password_email_webdav_issue.body:match(\"transaction_id=([^\\n]+)\"))\nlocal password_email_webdav_token = assert(password_email_webdav_issue.body:match(\"token=([^\\n]+)\"))\nlocal password_email_webdav_response = request_http(\"POST\", \"/_vectis/auth-password-email/webdav-key\", \"username=pack-email-user&pending_transaction_id=\" .. password_email_webdav_pending .. \"&email_transaction_id=\" .. password_email_webdav_transaction_id .. \"&email_token=\" .. password_email_webdav_token, {[\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(password_email_webdav_response.ok == true, password_email_webdav_response.error)\nassert(password_email_webdav_response.status == 200)\nassert_no_store(password_email_webdav_response)\nlocal password_email_webdav_client_id = assert(password_email_webdav_response.body:match(\"client_id=([^\\n]+)\"))\nlocal password_email_webdav_client_secret = assert(password_email_webdav_response.body:match(\"client_secret=([^\\n]+)\"))\nlocal password_email_webdav_basic_auth = \"Basic \" .. base64(password_email_webdav_client_id .. \":\" .. password_email_webdav_client_secret)\nlocal password_email_webdav_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = password_email_webdav_basic_auth})\nassert(password_email_webdav_status.ok == true, password_email_webdav_status.error)\nassert(password_email_webdav_status.status == 200)\nassert(password_email_webdav_status.body == '{\"ok\":true,\"surface\":\"pack\"}\\n')\nlocal wrong_email_login = request_http(\"POST\", \"/_vectis/auth/continue\","
       asset_script_body "${asset_script_body}")
string(REPLACE
       "totp_code=287082"
       "totp_code=\" .. totp_59 .. \""
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
       "assert(webdav_mutated.body == \"webdav mutated\\n\")\nlocal webdav_list = request_http(\"PROPFIND\", \"/dav/assets\", nil, {Authorization = basic_auth, Depth = \"1\"})\nassert(webdav_list.ok == true, webdav_list.error)\nassert(webdav_list.status == 207)\nassert(webdav_list.body:find(\"/dav/assets/app.txt\", 1, true))\nlocal webdav_mkcol = request_http(\"MKCOL\", \"/dav/user\", nil, {Authorization = basic_auth})\nassert(webdav_mkcol.ok == true, webdav_mkcol.error)\nassert(webdav_mkcol.status == 201)\nlocal webdav_copy = request_http(\"COPY\", \"/dav/assets/app.txt\", nil, {Authorization = basic_auth, Destination = \"http://127.0.0.1:28181/dav/user/copied.txt\"})\nassert(webdav_copy.ok == true, webdav_copy.error)\nassert(webdav_copy.status == 201)\nlocal webdav_copied = request_http(\"GET\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth})\nassert(webdav_copied.status == 200)\nassert(webdav_copied.body == \"webdav mutated\\n\")\nlocal webdav_move = request_http(\"MOVE\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth, Destination = \"http://127.0.0.1:28181/dav/user/moved.txt\"})\nassert(webdav_move.ok == true, webdav_move.error)\nassert(webdav_move.status == 201)\nlocal webdav_moved = request_http(\"GET\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_moved.status == 200)\nassert(webdav_moved.body == \"webdav mutated\\n\")\nlocal webdav_old_copy = request_http(\"GET\", \"/dav/user/copied.txt\", nil, {Authorization = basic_auth})\nassert(webdav_old_copy.status == 404)\nlocal webdav_delete = request_http(\"DELETE\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_delete.ok == true, webdav_delete.error)\nassert(webdav_delete.status == 204)\nlocal webdav_deleted = request_http(\"GET\", \"/dav/user/moved.txt\", nil, {Authorization = basic_auth})\nassert(webdav_deleted.status == 404)\nlocal logout_response = request_http(\"POST\", \"/_vectis/auth/logout\", \"\", {Authorization = basic_auth, [\"Content-Type\"] = \"application/x-www-form-urlencoded\"})\nassert(logout_response.ok == true, logout_response.error)\nassert(logout_response.status == 200)\nassert_no_store(logout_response)\nassert(logout_response.body == \"logged_out=1\\n\")\nlocal logged_out_status = request_http(\"GET\", \"/_vectis/status\", nil, {Authorization = basic_auth})\nassert(logged_out_status.status == 401)\nlocal logged_out_webdav = request_http(\"GET\", \"/dav/assets/app.txt\", nil, {Authorization = basic_auth})\nassert(logged_out_webdav.status == 401)\nlocal embedded_after_webdav = fetch_http(\"/assets/app.txt\")"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(server:stop() == true)\nserver:close()\nassert(vectis.embedded.extract({to = extract_to}) == true)"
       "assert(server:stop() == true)\nserver:close()\nassert(vectis.cert.generate_bundle({common_name = \"localhost\", ip_addresses = \"127.0.0.1\", output_bundle_path = https_cert_bundle, key_bits = 2048, valid_days = 1}) == true)\nlocal https_server = assert(vectis.server.new({bind = \"127.0.0.1\", port = 28182, tls = {mode = \"manual\", cert_key_bundle_path = https_cert_bundle, domain = \"localhost\"}}))\nassert(https_server:static_embedded({path_prefix = \"/\", cache_control = \"max-age=30\"}) == true)\nassert(https_server:start() == true)\nlocal https_response\nfor _ = 1, 20 do\n  https_response = curl.perform({url = \"https://localhost:28182/\", protocols = \"https\", timeout_ms = 2000, connect_timeout_ms = 1000, verify_peer = false, verify_host = false, no_signal = true})\n  if https_response.ok then break end\n  os.execute(\"sleep 0.1\")\nend\nassert(https_response.ok == true, https_response.error)\nassert(https_response.status == 200)\nassert(https_response.body:match(\"Acme Test\"))\nassert(https_response.headers:lower():find(\"cache-control: max-age=30\", 1, true))\nassert(https_server:stop() == true)\nhttps_server:close()\nassert(vectis.embedded.extract({to = extract_to}) == true)"
       asset_script_body "${asset_script_body}")
string(REPLACE
       "assert(vectis.embedded.extract({to = extract_to, policy = \"overwrite\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")"
       "assert(vectis.embedded.extract({to = extract_to, policy = \"overwrite\"}) == true)\nassert(read_file(extract_to .. \"/assets/app.txt\") == \"generic embedded asset\\n\")\nassert(read_file(extract_to .. \"/user-created.txt\") == \"user\\n\")"
       asset_script_body "${asset_script_body}")
string(REPLACE "28181" "${asset_http_port}" asset_script_body
       "${asset_script_body}")
string(REPLACE "28182" "${asset_https_port}" asset_script_body
       "${asset_script_body}")
string(REPLACE "28183" "${asset_disk_port}" asset_script_body
       "${asset_script_body}")
file(WRITE "${asset_script}" "${asset_script_body}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_invalid_path_output}" --asset "${asset_dir}/index.html=/../index.html"
                RESULT_VARIABLE asset_invalid_path_result
                OUTPUT_VARIABLE asset_invalid_path_stdout
                ERROR_VARIABLE asset_invalid_path_stderr)
if(asset_invalid_path_result EQUAL 0)
  message(FATAL_ERROR "pack invalid --asset logical path unexpectedly succeeded")
endif()
if(NOT asset_invalid_path_stderr MATCHES "invalid embedded asset path")
  message(FATAL_ERROR "pack invalid --asset logical path failed with unexpected error: ${asset_invalid_path_stdout}${asset_invalid_path_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_invalid_manifest_path_output}" --asset-manifest "${asset_invalid_path_manifest}"
                RESULT_VARIABLE asset_invalid_manifest_path_result
                OUTPUT_VARIABLE asset_invalid_manifest_path_stdout
                ERROR_VARIABLE asset_invalid_manifest_path_stderr)
if(asset_invalid_manifest_path_result EQUAL 0)
  message(FATAL_ERROR "pack invalid asset manifest logical path unexpectedly succeeded")
endif()
if(NOT asset_invalid_manifest_path_stderr MATCHES "invalid embedded asset path")
  message(FATAL_ERROR "pack invalid asset manifest logical path failed with unexpected error: ${asset_invalid_manifest_path_stdout}${asset_invalid_manifest_path_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_invalid_content_type_map_output}" --asset-dir "/:${asset_dir}" --content-type-map "${invalid_content_type_map}"
                RESULT_VARIABLE asset_invalid_content_type_map_result
                OUTPUT_VARIABLE asset_invalid_content_type_map_stdout
                ERROR_VARIABLE asset_invalid_content_type_map_stderr)
if(asset_invalid_content_type_map_result EQUAL 0)
  message(FATAL_ERROR "pack invalid content-type map unexpectedly succeeded")
endif()
if(NOT asset_invalid_content_type_map_stderr MATCHES "invalid content-type map extension")
  message(FATAL_ERROR "pack invalid content-type map failed with unexpected error: ${asset_invalid_content_type_map_stdout}${asset_invalid_content_type_map_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_symlink_reject_output}" --asset-dir "/:${asset_dir}" --content-type-map "${content_type_map}" --asset-manifest "${asset_manifest}" --extract-mode repair
                RESULT_VARIABLE asset_symlink_reject_result
                OUTPUT_VARIABLE asset_symlink_reject_stdout
                ERROR_VARIABLE asset_symlink_reject_stderr)
if(asset_symlink_reject_result EQUAL 0)
  message(FATAL_ERROR "pack asset directory symlink unexpectedly succeeded without --follow-symlinks")
endif()
if(NOT asset_symlink_reject_stderr MATCHES "requires --follow-symlinks")
  message(FATAL_ERROR "pack asset directory symlink failed with unexpected error: ${asset_symlink_reject_stdout}${asset_symlink_reject_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_symlink_asset_reject_output}" --asset "${asset_dir}/assets/app-link.txt=/assets/app-link.txt"
                RESULT_VARIABLE asset_symlink_asset_reject_result
                OUTPUT_VARIABLE asset_symlink_asset_reject_stdout
                ERROR_VARIABLE asset_symlink_asset_reject_stderr)
if(asset_symlink_asset_reject_result EQUAL 0)
  message(FATAL_ERROR "pack --asset symlink unexpectedly succeeded without --follow-symlinks")
endif()
if(NOT asset_symlink_asset_reject_stderr MATCHES "requires --follow-symlinks")
  message(FATAL_ERROR "pack --asset symlink failed with unexpected error: ${asset_symlink_asset_reject_stdout}${asset_symlink_asset_reject_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_symlink_manifest_reject_output}" --asset-manifest "${asset_symlink_manifest}"
                RESULT_VARIABLE asset_symlink_manifest_reject_result
                OUTPUT_VARIABLE asset_symlink_manifest_reject_stdout
                ERROR_VARIABLE asset_symlink_manifest_reject_stderr)
if(asset_symlink_manifest_reject_result EQUAL 0)
  message(FATAL_ERROR "pack asset manifest symlink unexpectedly succeeded without --follow-symlinks")
endif()
if(NOT asset_symlink_manifest_reject_stderr MATCHES "requires --follow-symlinks")
  message(FATAL_ERROR "pack asset manifest symlink failed with unexpected error: ${asset_symlink_manifest_reject_stdout}${asset_symlink_manifest_reject_stderr}")
endif()

execute_process(COMMAND "${VECTIS_BIN}" -a pack --script "${asset_script}" --output "${asset_output}" --asset-dir "/:${asset_dir}" --content-type-map "${content_type_map}" --asset-manifest "${asset_manifest}" --asset-manifest "${asset_symlink_manifest}" --extract-mode repair --follow-symlinks
                RESULT_VARIABLE asset_pack_result
                OUTPUT_VARIABLE asset_pack_stdout
                ERROR_VARIABLE asset_pack_stderr)
if(NOT asset_pack_result EQUAL 0)
  message(FATAL_ERROR "vectis -a pack with assets failed: ${asset_pack_stdout}${asset_pack_stderr}")
endif()

file(REMOVE_RECURSE "${asset_extract_dir}" "${asset_webdav_cache_dir}")
file(REMOVE "${asset_webdav_credentials}" "${asset_https_cert_bundle}" "${asset_smtp_mailbox}")
execute_process(COMMAND "${VECTIS_PACK_SMTP_HARNESS}" "${asset_output}" "${asset_smtp_mailbox}"
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
