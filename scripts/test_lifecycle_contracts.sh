#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version_path="$repo_root/VERSION"
version_work="$repo_root/build/test-release-version"
source_stage_dist="$version_work/source-dist"
luarocks_dist="$version_work/luarocks-dist"
untracked_source_probe="$repo_root/vectis-untracked-source-probe.txt"
saved_version=
had_version=0

cleanup() {
  if [ "$had_version" -eq 1 ]; then
    printf '%s' "$saved_version" >"$version_path"
  else
    rm -f "$version_path"
  fi
  rm -f "$untracked_source_probe"
  rm -rf "$version_work"
}
trap cleanup EXIT INT TERM

assert_contains() {
  file=$1
  pattern=$2
  if ! grep -Eq "$pattern" "$file"; then
    echo "missing lifecycle contract pattern in $file: $pattern" >&2
    exit 1
  fi
}

assert_not_contains() {
  file=$1
  pattern=$2
  if grep -Eq "$pattern" "$file"; then
    echo "forbidden lifecycle contract pattern in $file: $pattern" >&2
    exit 1
  fi
}

assert_host_debug_target() {
  host_system=$1
  host_machine=$2
  expected_target=$3
  expected_processor=$4
  output=$(
    VECTIS_DEPS_DRY_RUN=1 \
      VECTIS_HOST_UNAME_S="$host_system" \
      VECTIS_HOST_UNAME_M="$host_machine" \
      "$repo_root/scripts/deps.sh" deps-host-debug
  )

  if ! printf '%s\n' "$output" | grep -Eq "^target_id=$expected_target$"; then
    echo "deps-host-debug selected wrong target for $host_system $host_machine" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if ! printf '%s\n' "$output" |
    grep -Eq "^target_cmake_system_processor=$expected_processor$"; then
    echo "deps-host-debug selected wrong processor for $host_system $host_machine" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
}

assert_no_landed_test_assets() {
  if grep -RInE --exclude='test_lifecycle_contracts.sh' \
    '(\.\./landed|\blanded\b|\bLanded\b)' \
    "$repo_root/scripts" \
    "$repo_root/tests" \
    "$repo_root/examples" \
    "$repo_root/src" \
    "$repo_root/include" \
    "$repo_root/CMakeLists.txt"; then
    echo "runtime code and executable fixtures must not depend on Landed assets" >&2
    exit 1
  fi
}

assert_lua_examples_self_contained() {
  if grep -RInE \
    'require\((["'\''])(\.{1,2}/|examples[./]|examples/lua/|[^"'\'']*\.lua)\1\)' \
    "$repo_root/examples/lua"; then
    echo "Lua examples must be self-contained and must not require local example helper modules" >&2
    exit 1
  fi
}

assert_examples_do_not_shell_for_runtime_waits() {
  if grep -RInE \
    --include='*.c' \
    --include='*.lua' \
    '(os[.]execute|io[.]popen|(^|[^A-Za-z_])system[[:space:]]*[(]|/bin/(ba)?sh|sh[[:space:]]+-c|(^|[[:space:]])sleep[[:space:]]+[0-9])' \
    "$repo_root/examples"; then
    echo "Examples must not shell out or use external sleep/wait commands; use Vectis runtime wait helpers such as server:run(), server:wait(), or vectis.sleep_ms()" >&2
    exit 1
  fi
}

assert_action_surface_contract() {
  if grep -RInE --exclude='test_lifecycle_contracts.sh' \
    '(admin-operation|--admin-operation|--subcommand|pack[ _-]v2|pack[ _-]V2|Pack V2|VECTIS_PACK_V2|VECTIS_PACK2|PACK_V2)' \
    "$repo_root/docs" \
    "$repo_root/scripts" \
    "$repo_root/tests" \
    "$repo_root/examples" \
    "$repo_root/src" \
    "$repo_root/include" \
    "$repo_root/CMakeLists.txt" \
    "$repo_root/Makefile"; then
    echo "vectis command surface must stay on -a/--action with no pack V2 or admin-operation aliases" >&2
    exit 1
  fi
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis -a\|--action pack'
  assert_contains "$repo_root/src/vectis_cli.c" 'traces Lua line execution to stderr'
}

assert_lua_example_dx_contract() {
  if grep -RInE '#include[[:space:]]+"' "$repo_root/examples"; then
    echo "Examples must not include local shared helper headers" >&2
    exit 1
  fi
  assert_lua_examples_self_contained
  assert_examples_do_not_shell_for_runtime_waits
  if grep -RInE \
    '(^|[[:space:]])local[[:space:]]+function[[:space:]]+[A-Za-z_][A-Za-z0-9_]*|^[[:space:]]*function[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
    "$repo_root/examples/lua"; then
    echo "Lua examples must be self-contained and avoid named helper functions" >&2
    exit 1
  fi
  if grep -RInE 'rest\.(route|group)[[:space:]]*\(' "$repo_root/examples/lua"; then
    echo "Lua server examples must use direct server receiver route methods" >&2
    exit 1
  fi
  assert_contains "$repo_root/examples/README.md" 'Lua examples follow the same rule'
  assert_contains "$repo_root/examples/README.md" 'must not require another file from `examples/lua/`'
  assert_contains "$repo_root/examples/README.md" 'shell command wrappers for runtime waits'
}

assert_concurrency_mailbox_contract() {
  assert_contains "$repo_root/docs/lua.md" '\[Lua mailbox\]\(lua-mailbox\.md\)'
  assert_contains "$repo_root/docs/lua-mailbox.md" 'require\("vectis\.mailbox"\)'
  assert_contains "$repo_root/docs/lua-mailbox.md" 'box:pump'
  assert_contains "$repo_root/docs/lua-mailbox.md" 'owner-state callbacks'
  assert_contains "$repo_root/docs/lua-mailbox.md" 'not durable storage'
  assert_contains "$repo_root/docs/lua-mailbox.md" 'private Vectis runtime control bus'
  assert_contains "$repo_root/docs/concurrency-dx.md" 'T2 supervised runtimes'
  assert_contains "$repo_root/docs/concurrency-dx.md" 'ordinary `vectis_mailbox` handles remain process-local'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'Committed Decisions Before Later Service Families'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" '`vectis\.mailbox` pump contract'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'supervisor control channel remains internal'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'routes remain fail-closed with `503`'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'Kore listener preflight'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'request-body disk-spool preflight'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'Autoblock rule counts'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" 'vectis_kore_preflight_listener'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" 'vectis_kore_preflight_body_spool'
  assert_contains "$repo_root/src/vectis.c" 'server autoblock status_rule_count exceeds maximum'
  assert_contains "$repo_root/src/vectis.c" 'server autoblock max_entries exceeds maximum'
  assert_contains "$repo_root/include/vectis/vectis.h" 'size_t request_limit'
  assert_contains "$repo_root/include/vectis/vectis.h" 'access_log_path'
  assert_contains "$repo_root/include/vectis/vectis.h" 'kore_curl_timeout_seconds'
  assert_contains "$repo_root/include/vectis/vectis.h" 'kore_curl_recv_max_bytes'
  assert_contains "$repo_root/include/vectis/vectis.h" 'kore_quiet'
  assert_contains "$repo_root/src/vectis.c" 'server request_limit exceeds Kore uint32'
  assert_contains "$repo_root/src/vectis.c" 'server access_log_path must not be empty'
  assert_contains "$repo_root/src/vectis.c" 'server kore_curl_timeout_seconds must be 0 or at most'
  assert_contains "$repo_root/src/vectis_cli.c" '"request_limit"'
  assert_contains "$repo_root/src/vectis_cli.c" '"access_log_path"'
  assert_contains "$repo_root/src/vectis_cli.c" '"kore_curl_timeout_seconds"'
  assert_contains "$repo_root/src/vectis_cli.c" '"kore_curl_recv_max_bytes"'
  assert_contains "$repo_root/src/vectis_cli.c" '"kore_quiet"'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" \
    'http_request_limit = vectis_kore_u32_from_size\(server->request_limit\)'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" \
    'kore_quiet = server->kore_quiet \? 1 : 0'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" \
    'vectis_kore_preflight_access_log'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" 'kore_curl_timeout'
  assert_contains "$repo_root/src/vectis.c" 'vectis_static_inferred_content_type'
  assert_contains "$repo_root/tests/unit/test_vectis_config.c" \
    'vectis_internal_request_limit'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'text/javascript; charset=utf-8'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'vectis-runtime-access.log'
  assert_contains "$repo_root/tests/lua/http.cmake" \
    'content-type: text/css; charset=utf-8'
  assert_contains "$repo_root/tests/lua/http.cmake" \
    'content-type: application/octet-stream'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'request_limit = 32'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'access_log_path'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'kore_curl_timeout_seconds = 7'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'kore_curl_recv_max_bytes = 65536'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'kore_quiet = true'
  assert_contains "$repo_root/tests/unit/test_vectis_config.c" \
    'vectis_internal_kore_quiet'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" 'quiet=1'
  assert_contains "$repo_root/docs/lua-server.md" '`request_limit = 0`'
  assert_contains "$repo_root/docs/lua-server.md" 'access_log_path'
  assert_contains "$repo_root/docs/lua-server.md" '`kore_curl_timeout_seconds = 0`'
  assert_contains "$repo_root/docs/lua-server.md" '`kore_quiet = true`'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'Kore-quiet'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    '`vectis_server_config.kore_quiet`'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'vectis_server_config'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'server:upload'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'server:websocket'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'server:static_directory'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'vectis_register_static_file'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'vectis_register_websocket'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'vectis_request_kore'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'request_limit'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'worker_cpu_affinity'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'kore_quiet'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'pretty_error_pages'
  assert_contains "$repo_root/docs/kore-runtime-surface-audit.md" \
    'worker_death_policy'
  assert_contains "$repo_root/docs/lua-server.md" \
    'kore-runtime-surface-audit\.md'
  assert_contains "$repo_root/docs/api.md" 'Kore quiet mode'
  assert_contains "$repo_root/docs/lua-server.md" \
    'Vectis infers the response type'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Expose Kore'"'"'s active HTTP request-object limit as validated C and Lua `request_limit`'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Add extension-based content-type inference'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Expose an explicit C and Lua `access_log_path`'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Expose bounded C and Lua `kore_curl_timeout_seconds`'
  assert_contains "$repo_root/TODO.md" 'kore_curl_recv_max_bytes'
  assert_contains "$repo_root/TODO.md" '\[x\] Expose C and Lua `kore_quiet`'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Complete the remaining full Kore runtime configuration surface'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Support Kore features through Vectis where practical'
  assert_contains "$repo_root/include/vectis/vectis.h" \
    'VECTIS_AUTOBLOCK_MAX_STATUS_RULES'
  assert_contains "$repo_root/include/vectis/vectis.h" \
    'VECTIS_AUTOBLOCK_MAX_ENTRIES'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_kore_start_reports_occupied_listener'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_upload_server_rejects_spool_path'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'event rule name'
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Expose additional direct Kore configuration hooks where the startup lifecycle can report errors cleanly'
  assert_contains "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_supervised_routes_wait_for_full_app_readiness'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'docs/lua-mailbox\.md'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'box:pump'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'pump_callback_failures'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'direct Lua callbacks'
  assert_contains "$repo_root/examples/lua/mailbox_pipeline.lua" ':pump'
  assert_not_contains "$repo_root/docs/concurrency-dx.md" 'Kore-backed apps enter Kore directly'
  assert_not_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'Open Decisions Before Coding Beyond Lockdc'
}

assert_kore_lonejson_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'src/ljson\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'KORE_USE_LONEJSON'
  assert_contains "$repo_root/vendor/kore/upstream/src/ljson.c" 'lonejson_validate_reader'
  assert_contains "$repo_root/vendor/kore/upstream/src/ljson.c" 'lonejson_parse_reader'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_directory_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_order_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_auth_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'acme_ljson_parse\(&acme_badreq_map'
  assert_contains "$repo_root/vendor/kore/upstream/src/curl.c" 'defined\(__linux__\) && !defined\(KORE_VECTIS_NO_SECCOMP\)'
  assert_contains "$repo_root/vendor/kore/upstream/src/acme.c" 'defined\(__linux__\) && !defined\(KORE_VECTIS_NO_SECCOMP\)'
  assert_contains "$repo_root/vendor/kore/upstream/src/keymgr_openssl.c" 'defined\(__linux__\) && !defined\(KORE_VECTIS_NO_SECCOMP\)'
  assert_contains "$repo_root/vendor/kore/upstream/src/http.c" \
    '!has_content_type && media_type'
  assert_contains "$repo_root/vendor/kore/patches/series" \
    '0025-kore-vectis-preserve-fileref-content-type\.patch'
  assert_contains "$repo_root/vendor/kore/patches/0025-kore-vectis-preserve-fileref-content-type.patch" \
    '!has_content_type && media_type'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'S_SRC\+=src/ljson\.c'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'CFLAGS\+=-DKORE_USE_LONEJSON'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'LDFLAGS\+=-L\$\(LONEJSON_PATH\)/lib -llonejson'
}

assert_kore_static_runtime_contract() {
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Decide whether Kore remains patched in-tree'
  assert_contains "$repo_root/README.md" \
    '\[Kore runtime packaging\]\(docs/kore-runtime-packaging\.md\)'
  assert_contains "$repo_root/docs/api.md" \
    '\[Kore runtime packaging\]\(kore-runtime-packaging\.md\)'
  assert_contains "$repo_root/docs/kore-runtime-packaging.md" \
    'private Vectis-owned object runtime'
  assert_contains "$repo_root/docs/kore-runtime-packaging.md" \
    'standalone public `libkore` artifact'
  assert_contains "$repo_root/docs/kore-runtime-packaging.md" \
    '`vendor/kore/upstream`'
  assert_contains "$repo_root/docs/kore-runtime-packaging.md" \
    '`scripts/verify-kore-patches\.sh` verifies'
  assert_contains "$repo_root/CMakeLists.txt" \
    'add_library\(vectis_kore_runtime OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" \
    '\$<TARGET_OBJECTS:vectis_kore_runtime>'
  assert_contains "$repo_root/CMakeLists.txt" 'KORE_VECTIS_STATIC_RUNTIME'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" \
    'kore_vectis_runtime_symbols'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" \
    '"kore_parent_configure"'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" '"vectis_kore_route"'
  assert_contains "$repo_root/vendor/kore/upstream/src/module.c" \
    'kore_vectis_runtime_symbols'
  if grep -RInE 'kore_landed|Landed static runtime|landed_runtime' \
    "$repo_root/CMakeLists.txt" \
    "$repo_root/src" \
    "$repo_root/vendor/kore"; then
    echo "Vectis Kore static runtime must not retain Landed symbol names" >&2
    exit 1
  fi
}

assert_acme_lifecycle_contract() {
  assert_contains "$repo_root/TODO.md" \
    '\[x\] Add ACME lifecycle examples/tests beyond startup validation, using a controlled ACME test server'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_acme_mock_provider'
  assert_contains "$repo_root/tests/helpers/vectis_acme_mock_provider.c" \
    'tls-alpn-01'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'lua acme mock issuance'
  assert_contains "$repo_root/scripts/test-e2e.sh" \
    'VECTIS_ACME_STATE_PROVIDER="http://127\.0\.0\.1:\$acme_mock_port/directory"'
  assert_contains "$repo_root/scripts/test-e2e.sh" \
    'certificates/acme\.localhost\.test/fullchain\.pem'
  assert_contains "$repo_root/scripts/test-e2e.sh" \
    'https://acme\.localhost\.test:\$kore_acme_port/probe'
  assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-spec.md" \
    'deterministic e2e starts ACME mode against a'
}

assert_lockdc_lua_runtime_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'lua/vectis/lockd\.lua'
  assert_contains "$repo_root/CMakeLists.txt" 'share/lockdc-source/src/lua/lockdc_lua\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'share/lockdc-source/lua/lockdc/init\.lua'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_lockdc_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_lockdc_lua>'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "lockdc\.core"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "lockdc", vectis_lockdc_lua_init'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.lockd", vectis_lockd_lua_init'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("lockdc"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.lockd"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'lockdc\.version_string'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'encode_json = lockdc\.encode_json'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'decode_json = lockdc\.decode_json'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.lockd\.encode_json'
  assert_contains "$repo_root/docs/lua-lockd.md" 'vectis\.lockd\.native'
  assert_contains "$repo_root/docs/lua-lockd.md" 'client_bundle = "embedded"'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'client_bundle == "embedded"'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'embedded_lockd_bundle_source'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'local function lockdc_error'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'source_code = vstatus\.ERROR_SOURCE_LOCKDC'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'local function handler_error'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'source_code = vstatus\.ERROR_SOURCE_VECTIS'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M.with_client'
  assert_contains "$repo_root/docs/lua-lockd.md" 'err\.source = "lockdc"'
  assert_contains "$repo_root/docs/lua-lockd.md" 'err\.source = "vectis"'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'ERROR_SOURCE_LOCKDC'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'assert_status_error\(enqueue_failed_err'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'assert_status_error\(failed_err'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.save_json'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.load_json'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.enqueue_json'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.with_dequeued_json'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'lockdc\.open'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'vectis\.lockd\.open'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'client_bundle_source = assert\(vectis\.embedded_lockd_bundle_source\(\)\)'
  assert_contains "$repo_root/tests/lua/pack.cmake" 'server:consumer_service'
  assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_consumer_receiver_adapter'
  assert_contains "$repo_root/include/vectis/vectis.h" 'consumer_service_receiver'
  assert_contains "$repo_root/src/vectis.c" 'adapter.kind = "webdav_marker"'
  assert_contains "$repo_root/src/vectis.c" 'vectis_webdav_marker_receiver_create'
  assert_contains "$repo_root/src/vectis_cli.c" 'app->consumer_service_receiver'
  assert_not_contains "$repo_root/src/vectis_cli.c" 'consumer service handler.kind must be webdav_marker'
  assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_lockd_consumer_json_into'
  assert_contains "$repo_root/src/vectis.c" 'vectis_lockd_consumer_json_into'
  assert_contains "$repo_root/tests/unit/test_vectis_mailbox.c" 'test_lockd_consumer_json_into'
  assert_contains "$repo_root/docs/service-runtime-lifecycle-spec.md" 'bounded `vectis_lockd_consumer_json_into\(\)` payload decoding'
  assert_contains "$repo_root/docs/concurrency-dx.md" 'vectis_lockd_consumer_json_into\(message, max_payload_bytes, map, out, error\)'
  assert_contains "$repo_root/docs/api.md" 'bounded consumer JSON payload'
  assert_contains "$repo_root/TODO.md" '\[x\] Extend Vectis service-friendly lockd helpers only where they reduce real C workflow friction'
}

assert_lql_lua_runtime_contract() {
  assert_contains "$repo_root/scripts/deps.sh" 'lql_lua_archive="liblql-lua-\${lql_version}\.tar\.gz"'
  assert_contains "$repo_root/scripts/deps.sh" 'lql_lua_sha256="b440ce543586ebfc9aafd0e09a700126b9d62d85b8c34ae2ac19b0990db28438"'
  assert_contains "$repo_root/CMakeLists.txt" 'share/liblql-lua-source/lua/lql/init\.lua'
  assert_contains "$repo_root/CMakeLists.txt" 'share/liblql-lua-source/lua/lql_core\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_liblql_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_liblql_lua>'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "lql\.core"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "lql", vectis_liblql_lua_init'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("lql"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("lql\.core"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'lql\.version'
}

assert_opcua_lua_runtime_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'src/vectis_opcua_lua\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_opcua_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_opcua_lua>'
  assert_contains "$repo_root/CMakeLists.txt" 'cpkt::opcua'
  assert_contains "$repo_root/Makefile" 'test-opcua-lua-surface'
  assert_contains "$repo_root/Makefile" 'scripts/verify_opcua_lua_surface\.py'
  assert_contains "$repo_root/Makefile" 'test-opcua-pubsub-live'
  assert_contains "$repo_root/Makefile" 'scripts/test-live-opcua-pubsub\.sh'
  assert_contains "$repo_root/scripts/test-live-opcua-pubsub.sh" 'VECTIS_OPCUA_PUBSUB_LIVE'
  assert_contains "$repo_root/scripts/test-live-opcua-pubsub.sh" 'server:add_mqtt_pubsub_connection'
  assert_contains "$repo_root/scripts/test-live-opcua-pubsub.sh" 'opcua_pubsub_live=ok'
  assert_contains "$repo_root/scripts/verify_opcua_lua_surface.py" 'unexpected_missing'
  assert_contains "$repo_root/scripts/verify_opcua_lua_surface.py" 'client_pubsub'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "opcua"'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'luaopen_opcua'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_connect'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_object'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_variable_under'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_object_type'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_reference_type'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_reference'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_reference_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_delete_reference_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_browse_name'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write_display_name'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_access_level_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_minimum_sampling_interval'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_array_dimensions'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write_array_dimensions'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_data_value'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_history_read_raw'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_boolean_array_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_integer_array_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_localized_text_array_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write_index_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_browse_children_page'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_browse_next'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_call_method'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_method_argument'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_translate_browse_path'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_create_subscription'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_monitor_value'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_monitor_value_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_delete_monitored_item'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_data_change_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_monitor_events'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_monitor_event_fields'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_event_fields_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_browse_children_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_call_method_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_object_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_add_variable_async'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_async_call_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_async_browse_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_async_node_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_expanded_node_id_parse'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_expanded_node_id_print'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_new'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_variable'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_object_type'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_reference'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_reference_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_delete_reference_ex'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_display_name'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_array_dimensions'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_write_array_dimensions'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_data_value'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_integer_array_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_localized_text_array'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_write_index_range'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_browse_children_page'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_browse_next'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_method'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_read_method_argument'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_translate_browse_path'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_create_event'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_trigger_event'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_mqtt_pubsub_connection'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_published_dataset'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_published_variable'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_pubsub_writer_group'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_pubsub_data_set_writer'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_pubsub_reader_group'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_add_pubsub_data_set_reader'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_write_pubsub_configuration'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_load_pubsub_configuration'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_iterate'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" '\{"server", vectis_opcua_lua_server_new\}'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_opcua_e2e'
  assert_contains "$repo_root/tests/CMakeLists.txt" '\$<TARGET_FILE:vectis_bin>'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'run_packed_lua_contract'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'OPCUA_ENDPOINT'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'OPCUA_LUA_SERVER_PORT'
  assert_contains "$repo_root/TODO.md" '\[x\] Add packed e2e coverage for the OPC UA Lua client example'
  assert_contains "$repo_root/TODO.md" '\[x\] Define the OPC UA Lua callback, ownership, and Vectis concurrency contract'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server foundation bindings'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server data-value, full array value/read families'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server/client materialized browse and paged browse result bindings'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server method registration/callback retention'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server PubSub/MQTT configuration wrappers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client synchronous data-value'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client full array-family read/range bindings'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client common attribute and remote node-management parity'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua expanded-node-id constructors'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client/server array-dimension attribute read/write helpers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client subscription lifecycle and value-monitor callback bindings'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client compact event and selected event-field monitor callbacks'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client async read, write, and method-call callbacks'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client async browse and async object/variable creation callbacks'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local;integration"'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'cpkt_opcua_server_startup'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'cpkt_opcua_server_add_method'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'cpkt_lua_runtime_register_c_module'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'client:write'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("opcua"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.node_id_numeric'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.expanded_node_id_local'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.expanded_node_id_server_uri'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.value_string'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.value_integer_array'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.value_guid_array'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.client'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.server'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.NODE_CLASS_OBJECT'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.BROWSE_RESULT_ALL'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.MONITORING_REPORTING'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.DEADBAND_NONE'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'STATUS_BAD_USER_ACCESS_DENIED'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'METHOD_ARGUMENT_INPUT'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'set_default_security'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'set_default_encryption'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'endpoint_count'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'find_servers'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'read_data_value'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'history_read_raw'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'read_localized_text_array'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'browse_children_page'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'add_mqtt_pubsub_connection'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'write_pubsub_configuration'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'call_method_many'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.read_localized_text_array_range'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.add_variable_under'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.add_reference_type'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.add_reference_ex'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.read_browse_name'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.read_access_level_ex'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.read_array_dimensions'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.create_subscription'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.monitor_value_ex'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.monitor_event_fields'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.browse_children_async'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.call_method_async'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.add_object_async'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'client\.add_variable_async'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'write_array_dimensions'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_object_type'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_reference'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_reference_ex'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:delete_reference_ex'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_array_dimensions'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:write_array_dimensions'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_display_name'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_data_value'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_integer_array_range'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_localized_text_array'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:create_event'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:trigger_event'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:browse_children_page'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_mqtt_pubsub_connection'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_published_dataset'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_pubsub_data_set_reader'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:write_pubsub_configuration'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'set_default_security'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'set_default_encryption'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'STATUS_BAD_USER_ACCESS_DENIED'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'bad OPC UA credentials should be rejected'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'access-control callback should observe denied and accepted'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:add_method'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'server:read_method_argument'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:add_variable_under'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:add_reference_type'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:add_reference_ex'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_browse_name'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_access_level_ex'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_array_dimensions'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:write_array_dimensions'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:history_read_raw'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:delete_reference'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_integer_array_range'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_boolean_array_range'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_localized_text_array_range'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:browse_children_page'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:call_method_many'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:translate_browse_path'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'discovery_client:endpoint_count'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'discovery_client:find_servers'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:create_subscription'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:monitor_value_ex'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:delete_monitored_item'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'intentional opcua monitor failure'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:monitor_events'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:monitor_event_fields'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client monitor_event_fields callback should observe fixture event'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:read_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:write_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:browse_children_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:call_method_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:add_object_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'client:add_variable_async'
  assert_contains "$repo_root/examples/lua/opcua_client.lua" 'intentional opcua async failure'
  assert_contains "$repo_root/docs/lua.md" '\[Lua OPC UA\]\(lua-opcua\.md\)'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Array values: `value_boolean_array'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:read_data_value'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:read_data_value'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:history_read_raw'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:read_boolean_array'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:read_localized_text_array_range'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:add_reference_type'
  assert_contains "$repo_root/docs/lua-opcua.md" 'expanded_node_id_server_uri'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:add_reference_ex'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:read_access_level_ex'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:read_array_dimensions'
  assert_contains "$repo_root/docs/lua-opcua.md" 'browse_children_page'
  assert_contains "$repo_root/docs/lua-opcua.md" 'continuation_point'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:add_method'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:add_mqtt_pubsub_connection'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:write_pubsub_configuration'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:set_access_control'
  assert_contains "$repo_root/docs/lua-opcua.md" 'server:set_default_security'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:set_default_encryption'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:endpoint_count'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:find_servers'
  assert_contains "$repo_root/docs/lua-opcua.md" 'methods use 1-based Lua indexes'
  assert_contains "$repo_root/docs/lua-opcua.md" 'The facade does not read files implicitly'
  assert_contains "$repo_root/docs/lua-opcua.md" 'The callback is retained by the server'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Server PubSub helpers mirror cpkt'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:call_method_many'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:monitor_value_ex'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Value-monitor callbacks receive owned Lua tables'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:monitor_event_fields'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Event-monitor callbacks follow the same retention and error rules'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:call_method_async'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:browse_children_async'
  assert_contains "$repo_root/docs/lua-opcua.md" 'client:add_variable_async'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Async client callbacks are retained by the client until completion'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Method callbacks registered'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Every Lua-created OPC UA client or server is owned by the `lua_State`'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Background threads must not call Lua callbacks directly'
  assert_contains "$repo_root/docs/lua-opcua.md" 'Kore serving and lockd consumption can run simultaneously'
  assert_contains "$repo_root/docs/lua-opcua.md" 'all non-native client/server symbols'
  assert_contains "$repo_root/docs/lua-opcua.md" 'no client-side PubSub symbols'
  assert_contains "$repo_root/docs/lua-opcua.md" 'VECTIS_OPCUA_PUBSUB_LIVE=1 make'
  assert_contains "$repo_root/docs/lua-opcua.md" 'C-Only Native-Pointer Surfaces'
  assert_contains "$repo_root/docs/lua-opcua.md" 'cpkt_opcua_client_native_config'
  assert_contains "$repo_root/docs/lua-opcua.md" 'cpkt_opcua_server_native_config'
  assert_contains "$repo_root/docs/lua-opcua.md" 'cpkt_opcua_client_read_native_variant'
  assert_contains "$repo_root/docs/lua-opcua.md" 'cpkt_opcua_server_read_native_data_value'
  assert_contains "$repo_root/docs/lua-opcua.md" 'borrowed native open62541 pointers'
  assert_contains "$repo_root/docs/lua-opcua.md" 'must not be exposed as Lua userdata'
  assert_contains "$repo_root/docs/lua-opcua.md" 'pointer values'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'type/view/reference remote node-management'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'expanded-node references'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'array-dimension attributes'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'value-monitor callbacks'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'selected event-field callbacks'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'async client read/write/browse/method/node-create callbacks'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'server PubSub/MQTT graph wrappers'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'server access-control callbacks'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'default security byte-material configuration'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'endpoint and FindServers discovery'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'client raw history reads'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'surface audit enforces'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'no client PubSub symbols'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'make test-opcua-pubsub-live'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" 'native-pointer surfaces are documented C-only exclusions'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" '\| dep:opcua \| cpkt-opcua \| yes \| n/a \| yes \| yes \| yes \|'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" '\| workflow:opcua-client \| OPC UA client workflows \| yes \| n/a \| yes \| yes \| n/a \|'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" '\| workflow:opcua-server \| OPC UA server workflows \| yes \| n/a \| yes \| yes \| yes \|'
  assert_contains "$repo_root/docs/lua-coverage-matrix.md" '\| workflow:opcua-async \| OPC UA async/subscription/PubSub workflows \| yes \| n/a \| yes \| yes \| yes \|'
  assert_not_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_.*native'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_set_access_control_callback'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'vectis_opcua_lua_access_control_cb'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_server_set_default_security'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_set_default_encryption'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_get_endpoint_count'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_find_server_count'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua server access-control callback retention'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client/server default security byte-material configuration'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client endpoint and FindServers discovery helpers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add OPC UA Lua client raw history reads'
  assert_contains "$repo_root/TODO.md" '\[x\] Complete the dependency-native OPC UA Lua server facade over all non-native'
  assert_contains "$repo_root/TODO.md" '\[x\] Resolve OPC UA Lua client PubSub scope'
  assert_contains "$repo_root/TODO.md" '\[x\] Add opt-in live OPC UA server PubSub/MQTT broker validation'
  assert_contains "$repo_root/TODO.md" '\[x\] Document OPC UA Lua native-pointer C-only exclusions'
}

assert_audio_sus_lua_runtime_contract() {
  assert_contains "$repo_root/CMakeLists.txt" 'src/vectis_audio_lua\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'src/vectis_sus_lua\.c'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_audio_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" 'add_library\(vectis_sus_lua OBJECT'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_audio_lua>'
  assert_contains "$repo_root/CMakeLists.txt" '\$<TARGET_OBJECTS:vectis_sus_lua>'
  assert_contains "$repo_root/CMakeLists.txt" 'cpkt::audio'
  assert_contains "$repo_root/CMakeLists.txt" 'cpkt::sus'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "audio"'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "sus"'
  assert_contains "$repo_root/src/vectis_audio_lua.c" 'luaopen_audio'
  assert_contains "$repo_root/src/vectis_audio_lua.c" 'cpkt_audio_decoder_open_reader'
  assert_contains "$repo_root/src/vectis_audio_lua.c" 'cpkt_audio_encoder_open_writer'
  assert_contains "$repo_root/src/vectis_audio_lua.c" 'cpkt_audio_capture_open_default'
  assert_contains "$repo_root/src/vectis_audio_lua.c" 'cpkt_audio_playback_open_default'
  assert_contains "$repo_root/src/vectis_sus_lua.c" 'luaopen_sus'
  assert_contains "$repo_root/src/vectis_sus_lua.c" 'cpkt_sus_model_catalog_default'
  assert_contains "$repo_root/src/vectis_sus_lua.c" 'cpkt_sus_open_cached'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"audio"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"sus"'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("audio"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("sus"\)'
  assert_contains "$repo_root/tests/lua/audio_sus.cmake" 'audio\.decoder\.open_reader'
  assert_contains "$repo_root/tests/lua/audio_sus.cmake" 'audio\.capture\.open_default'
  assert_contains "$repo_root/tests/lua/audio_sus.cmake" 'sus\.open_cached'
  assert_contains "$repo_root/scripts/test-live-sus-audio.sh" 'VECTIS_LUA_SUS_MODEL_PATH'
  assert_contains "$repo_root/scripts/test-live-sus-audio.sh" 'sus_audio_loaded_model_live=ok'
  assert_contains "$repo_root/scripts/test-hardening-sus-audio.sh" 'VECTIS_SUS_AUDIO_HARDENING'
  assert_contains "$repo_root/scripts/test-hardening-sus-audio.sh" 'sus_audio_hardening=ok'
  assert_contains "$repo_root/Makefile" 'test-sus-audio-live'
  assert_contains "$repo_root/Makefile" 'test-sus-audio-hardening'
  assert_contains "$repo_root/Makefile" 'prerelease-hardening: prerelease test-sus-audio-hardening'
  assert_contains "$repo_root/examples/lua/audio_devices.lua" 'VECTIS_LUA_AUDIO_DEVICE_EXAMPLE'
  assert_contains "$repo_root/examples/lua/sus_loaded_model.lua" 'VECTIS_LUA_SUS_MODEL_PATH'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_audio_devices'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_sus_loaded_model'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'examples/lua/audio_sus\.lua'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'examples/lua/sus_loaded_model\.lua'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'examples/lua/audio_devices\.lua'
  assert_contains "$repo_root/TODO.md" '\[x\] Add deterministic Lua example and packed coverage for opt-in audio capture/playback device workflows'
}

assert_lua_coverage_matrix_contract() {
  matrix="$repo_root/docs/lua-coverage-matrix.md"
  lua_index="$repo_root/docs/lua.md"

  assert_contains "$matrix" 'The `vectis` executable is a primary product surface'
  assert_contains "$matrix" '\[`docs/lua\.md`\]\(lua\.md\)'
  assert_contains "$matrix" '\| dep:lua-runtime \|'
  assert_contains "$matrix" '\| dep:lockdc \|'
  assert_contains "$matrix" '\| dep:lonejson \|'
  assert_contains "$matrix" '\| dep:pslog \|'
  assert_contains "$matrix" '\| dep:lql \|'
  assert_contains "$matrix" '\| dep:cai \|'
  assert_contains "$matrix" '\| dep:libmdf \|'
  assert_contains "$matrix" '\| dep:softline \|'
  assert_contains "$matrix" '\| dep:curl \|'
  assert_contains "$matrix" '\| dep:openssl \|'
  assert_contains "$matrix" '\| dep:libssh2 \|'
  assert_contains "$matrix" '\| dep:libxml2 \|'
  assert_contains "$matrix" '\| dep:dsv \|'
  assert_contains "$matrix" '\| dep:dsv \| Vectis DSV/CSV/TSV helpers \| yes \| yes \| yes \| yes \|'
  assert_contains "$matrix" '\| dep:opcua \|'
  assert_contains "$matrix" 'server lifecycle, node IDs, expanded node IDs, scalar and array values, data values'
  assert_contains "$matrix" '\| dep:sus \|'
  assert_contains "$matrix" '\| dep:audio \|'
  assert_contains "$matrix" '\| dep:nghttp2 \|'
  assert_contains "$matrix" '\| dep:zlib \|'
  assert_contains "$matrix" '\| dep:miniaudio \|'

  assert_contains "$matrix" '\| workflow:server-runtime \|'
  assert_contains "$matrix" '\| workflow:routes-json \|'
  assert_contains "$matrix" '\| workflow:routes-streaming \|'
  assert_contains "$matrix" '\| workflow:libs \|'
  assert_contains "$repo_root/docs/lua-server.md" 'server:route'
  assert_contains "$repo_root/docs/lua-server.md" 'server:dsv'
  assert_contains "$matrix" '\| workflow:static-assets \|'
  assert_contains "$matrix" '\| workflow:webdav-server \|'
  assert_contains "$matrix" '\| workflow:webdav-client \|'
  assert_contains "$matrix" '\| workflow:auth \|'
  assert_contains "$matrix" '\| workflow:certs \|'
  assert_contains "$matrix" 'vectis_cert_inspect_bundle'
  assert_contains "$matrix" '\| workflow:http-client \|'
  assert_contains "$matrix" '\| workflow:sftp-curl \|'
  assert_contains "$matrix" '\| workflow:ssh-exec \|'
  assert_contains "$matrix" '\| workflow:scp \|'
  assert_contains "$matrix" '\| workflow:sftp-libssh2 \|'
  assert_contains "$matrix" '\| workflow:xml \|'
  assert_contains "$matrix" 'vectis_xml_write_lonejson'
  assert_contains "$matrix" 'vectis_xml_lonejson_to_bytes'
  assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_xml_write_lonejson'
  assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_xml_lonejson_to_bytes'
  assert_contains "$repo_root/src/vectis.c" 'vectis_xml_write_lonejson'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_xml_serialize'
  assert_contains "$repo_root/lua/vectis/xml.lua" 'function M\.serialize'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'xml\.serialize'
  assert_contains "$repo_root/docs/lua-xml.md" 'vectis_xml_write_lonejson'
  assert_contains "$repo_root/TODO.md" '\[x\] Add XML serialization coverage'
  assert_contains "$matrix" '\| workflow:dsv \|'
  assert_contains "$matrix" '\| workflow:dsv \| CSV/TSV/DSV parse/serialize workflows \| yes \| yes \| yes \| yes \|'
  assert_contains "$matrix" '\| workflow:lockd-state \|'
  assert_contains "$matrix" '\| workflow:lockd-queue \|'
  assert_contains "$matrix" '\| workflow:server-consumer \|'
  assert_contains "$matrix" '\| workflow:opcua-client \|'
  assert_contains "$matrix" 'Local server-backed normal and packed e2e exists'
  assert_contains "$matrix" 'basic remote object/variable creation/deletion'
  assert_contains "$matrix" '\| workflow:opcua-server \|'
  assert_contains "$matrix" 'full array value/read families, numeric range writes'
  assert_contains "$matrix" '\| workflow:opcua-async \|'
  assert_contains "$matrix" '\| workflow:cai \|'
  assert_contains "$matrix" 'Dependency-native CAI local workflows are packed-tested'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'examples/lua/cai_local\.lua'
  assert_contains "$matrix" '\| workflow:sus \|'
  assert_contains "$matrix" '\| workflow:audio \|'
  assert_contains "$matrix" '\| workflow:terminal-agent \|'
  assert_contains "$matrix" '\| workflow:pack \|'
  assert_contains "$matrix" '\| workflow:totp-qr \|'
  assert_contains "$matrix" '\| workflow:openapi \|'
  assert_contains "$matrix" '`server:openapi_doc\(\)` attaches route metadata'

  assert_contains "$matrix" 'New bundled dependencies that are useful from app code must add a `dep:\*`'
  assert_contains "$matrix" 'New Vectis C SDK workflows must add a `workflow:\*`'
  assert_contains "$matrix" 'New Lua module documentation must be linked from `docs/lua\.md`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a Lua coverage matrix under `docs/`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a lifecycle contract that fails when a bundled dependency'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a checked Lua facade documentation index'
  assert_contains "$repo_root/TODO.md" '\[x\] Expose the full public Vectis status enum in Lua'
  assert_contains "$repo_root/TODO.md" '\[x\] Add checked Lua facade conventions'
  assert_contains "$repo_root/TODO.md" 'executable Lua facade matrix contract'
  assert_contains "$repo_root/TODO.md" 'executable Lua facade behavior coverage audit'
  assert_contains "$repo_root/Makefile" '^test-lua-facade-matrix:'
  assert_contains "$repo_root/Makefile" 'scripts/test_lua_facade_matrix_contracts\.sh'
  assert_contains "$repo_root/Makefile" '^test-lua-facade-behavior:'
  assert_contains "$repo_root/Makefile" 'scripts/test_lua_facade_behavior_coverage\.sh'
  assert_contains "$repo_root/scripts/test_lua_facade_matrix_contracts.sh" \
    'lua facade matrix contracts ok'
  assert_contains "$repo_root/scripts/test_lua_facade_matrix_contracts.sh" \
    'vectis.libs'
  assert_contains "$repo_root/scripts/test_lua_facade_behavior_coverage.sh" \
    'local_behavior=\('
  assert_contains "$repo_root/scripts/test_lua_facade_behavior_coverage.sh" \
    'workflow_behavior=\('
  assert_contains "$repo_root/scripts/test_lua_facade_behavior_coverage.sh" \
    'packed_behavior=\('
  assert_contains "$repo_root/scripts/test_lua_facade_behavior_coverage.sh" \
    'streaming responses cannot be retried safely'
  assert_contains "$repo_root/tests/CMakeLists.txt" \
    'vectis_lua_facade_behavior_coverage'
  assert_contains "$repo_root/tests/lua/smoke.lua" \
    'package.loaded\["vectis.cai"\] == vcai'
  assert_contains "$repo_root/TODO.md" '\[x\] Add Lua OpenAPI route metadata'
  assert_contains "$repo_root/TODO.md" '\[x\] Add explicit file-backed Lua `spooled_source` route responses'
  assert_contains "$repo_root/TODO.md" '\[x\] Add Lua request-body streaming policies through a dedicated `server:upload\(\)` helper'
  assert_contains "$repo_root/TODO.md" '\[x\] Add packed API example coverage for live Lua `stream_source`, `server:sse`, and true request-body streaming upload routes'
  assert_contains "$repo_root/TODO.md" 'live `stream_source`/`server:sse` responses'
  assert_contains "$matrix" 'file-backed `spooled_source` responses'
  assert_contains "$matrix" 'live callback-backed `stream_source` responses'
  assert_contains "$matrix" '`server:upload` provides true request-body streaming'
  assert_contains "$matrix" '`server:sse` provides live SSE framing'
  assert_contains "$matrix" 'packable Lua API example exercises stream, SSE, and upload routes'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_server_upload_dispatch'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_server_upload'
  assert_contains "$repo_root/src/vectis_cli.c" 'body_streaming_upload'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_response_callback_source'
  assert_contains "$repo_root/src/vectis_cli.c" '"stream_source"'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_server_sse'
  assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_response_stream_source'
  assert_contains "$repo_root/src/vectis_kore_bridge.c" 'transfer-encoding", "chunked"'
  assert_contains "$repo_root/docs/lua-server.md" '`spooled_source` is a file-backed response source'
  assert_contains "$repo_root/docs/lua-server.md" '`stream_source` is a live response source'
  assert_contains "$repo_root/docs/lua-server.md" '`server:upload\(opts\)` registers a route'
  assert_contains "$repo_root/docs/lua-server.md" '`server:sse\(opts\)` registers a GET route'
  assert_contains "$repo_root/tests/lua/http.cmake" '/dynamic-stream'
  assert_contains "$repo_root/tests/lua/http.cmake" '/dynamic-upload'
  assert_contains "$repo_root/tests/lua/http.cmake" '/dynamic-sse'
  assert_contains "$repo_root/tests/lua/http.cmake" '/dynamic-spooled'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_server_openapi_doc'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_server_openapi_schema_refs_free_all'
  assert_contains "$repo_root/src/vectis_cli.c" 'openapi_schema_refs'
  assert_contains "$repo_root/docs/lua-server.md" 'server:openapi_doc'
  assert_contains "$repo_root/docs/lua-server.md" 'server:openapi'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'openapi_server:openapi'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'collectgarbage\("collect"\)'
  assert_contains "$repo_root/examples/lua/api_server.lua" '/openapi\.json'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:openapi'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'stream_source'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:sse'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:upload'
  assert_contains "$repo_root/TODO.md" '\[x\] Add user-facing Lua docs and direct JSON passthrough helpers for `vectis\.lockd`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add narrow `vectis\.lockd` workflow helpers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add narrow `vectis\.lockd` JSON state save/load helpers'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M\.enqueue_json'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M\.load_json'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M\.save_json'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M\.with_acquired_lease'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M\.with_dequeued_json'
  assert_contains "$repo_root/docs/lua-lockd.md" 'with_acquired_lease'
  assert_contains "$repo_root/docs/lua-lockd.md" 'with_dequeued_json'
  assert_contains "$repo_root/docs/lua-lockd.md" 'enqueue_json'
  assert_contains "$repo_root/docs/lua-lockd.md" 'load_json'
  assert_contains "$repo_root/docs/lua-lockd.md" 'save_json'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'native = lockdc'
  assert_contains "$repo_root/docs/lua-lockd.md" 'Use direct `require\("lockdc"\)` or `vectis\.lockd\.native`'
  assert_contains "$repo_root/docs/lua-lockd.md" 'complete lockd API'
  assert_contains "$repo_root/docs/lua-lockd.md" 'queue ack/nack/extend'
  assert_contains "$repo_root/TODO.md" '\[x\] Provide Lua helpers for document store, retrieval, query, leases, enqueue, dequeue, ack/nack'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'native_client:acquire'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" '"query_raw"'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" '"queue_nack"'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" '"payload_json"'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'lockd\.load_json'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'lockd\.save_json'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'lockd\.with_dequeued_json'
  assert_contains "$repo_root/tests/lua/lockd_helpers.cmake" 'vectis-lockd-helpers-ok'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_lockd_helpers'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.save_json'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.load_json'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.enqueue_json'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.with_dequeued_json'
  assert_contains "$repo_root/tests/lua/lockd_examples.cmake" 'vectis-lockd-examples-ok'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_lockd_examples'
  assert_contains "$matrix" 'with_acquired_lease'
  assert_contains "$matrix" 'vectis\.lockd\.enqueue_json'
  assert_contains "$matrix" 'load_json'
  assert_contains "$matrix" 'save_json'
  assert_contains "$matrix" 'with_dequeued_json'
  assert_contains "$repo_root/TODO.md" 'simple `get`, `post`, `put`, `patch`, `delete`, and `head` helpers'
  assert_not_contains "$repo_root/TODO.md" 'Kore, OpenSSL certificate workflows, and broader libssh2'
  assert_contains "$repo_root/TODO.md" '\[x\] Expand `vectis\.cert` Lua helpers to cover private key generation'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a Vectis-owned Lua `vectis\.kore` module'
  assert_contains "$repo_root/TODO.md" '\[x\] Add broader Lua libssh2 facades'
  assert_contains "$repo_root/TODO.md" '\[x\] Add one-shot Lua libssh2-backed SFTP filesystem helpers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add broader Lua libssh2-backed SFTP session, file open/read/write/stat, and directory iteration handles'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a reusable Lua SSH receiver over the public C `vectis_ssh` receiver'
  assert_contains "$repo_root/TODO.md" '\[x\] Add packed e2e coverage for Lua SSH command execution and known_hosts rejection'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a Lua example for stateful SFTP/libssh2 lower-level handle operations'
  assert_contains "$repo_root/docs/lua-certs.md" 'require\("openssl"\)'
  assert_contains "$repo_root/TODO.md" '\[x\] Extend lower-level OpenSSL Lua access only where concrete signing, verification, digest, encoding, or key-inspection workflows require it'
  assert_contains "$repo_root/TODO.md" '\[x\] Keep dependency-native OpenSSL exposure narrow'
  assert_contains "$repo_root/docs/lua-openssl.md" 'current lower-level surface is limited'
  assert_contains "$repo_root/docs/lua-openssl.md" 'Do not add Lua bindings for OpenSSL object lifetime management'
  assert_contains "$repo_root/tests/lua/openssl.cmake" 'openssl\.digest_hex\("sha256"'
  assert_contains "$repo_root/tests/lua/openssl.cmake" 'openssl\.hmac_hex\("sha256"'
  assert_contains "$repo_root/tests/lua/openssl.cmake" 'openssl\.sign'
  assert_contains "$repo_root/tests/lua/openssl.cmake" 'openssl\.verify'
  assert_contains "$repo_root/tests/lua/openssl.cmake" 'openssl\.random_bytes'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_stat'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_chmod'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_open'
  assert_contains "$repo_root/docs/lua-ssh.md" 'vectis\.ssh\.open'
  assert_contains "$repo_root/docs/lua-ssh.md" 'session:exec'
  assert_contains "$repo_root/docs/lua-ssh.md" 'session:sftp_open'
  assert_contains "$repo_root/docs/lua-ssh.md" 'session:open_file'
  assert_contains "$repo_root/docs/lua-ssh.md" 'SFTP_OPEN_READ'
  assert_contains "$repo_root/docs/lua-ssh.md" 'dir:read'
  assert_contains "$repo_root/docs/lua-ssh.md" 'host_key_sha256'
  assert_contains "$repo_root/docs/lua-ssh.md" 'SHA256:<base64>'
  assert_contains "$repo_root/src/vectis_cli.c" 'VECTIS_LUA_SSH_SESSION'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_open'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_session_exec'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_session_sftp_open'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_stat'
  assert_contains "$repo_root/src/vectis_cli.c" 'sftp_chmod'
  assert_contains "$repo_root/src/vectis_cli.c" 'SFTP_OPEN_TRUNCATE'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_open'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_session_open_file'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_dir_read'
  assert_contains "$repo_root/src/vectis_cli.c" 'host_key_sha256'
  assert_contains "$repo_root/tests/lua/ssh_sftp.cmake" 'sftp_stat'
  assert_contains "$repo_root/tests/lua/ssh_sftp.cmake" 'sftp_open'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'ssh\.SFTP_OPEN_READ'
  assert_contains "$repo_root/tests/lua/ssh_sftp.cmake" 'vectis\.ssh\.open'
  assert_contains "$repo_root/examples/lua/ssh_command.lua" 'vectis\.ssh\.open'
  assert_contains "$repo_root/examples/lua/ssh_command.lua" 'session:exec'
  assert_contains "$repo_root/examples/lua/ssh_command.lua" 'VECTIS_LUA_SSH_HOST_KEY_SHA256'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'packed lua ssh command'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'Packed Lua libssh2 SSH unexpectedly accepted mismatched known_hosts pin'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'ssh command accepts SHA-256 host-key fingerprint pin'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'Lua libssh2 SSH unexpectedly accepted mismatched host-key fingerprint'
  assert_contains "$repo_root/examples/lua/sftp_handles.lua" 'sftp_open'
  assert_contains "$repo_root/examples/lua/sftp_handles.lua" 'open_file'
  assert_contains "$repo_root/examples/lua/sftp_handles.lua" 'SFTP_OPEN_WRITE'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'lua stateful sftp handles'
  assert_contains "$matrix" 'sftp_stat'
  assert_contains "$matrix" 'vectis\.ssh\.open'
  assert_contains "$matrix" 'stateful SFTP session/file/directory receivers exist'
  assert_contains "$matrix" 'SFTP_OPEN_\*'
  assert_contains "$matrix" 'packed SSH command and SCP validation coverage exists'
  assert_contains "$matrix" 'SHA-256 host-key fingerprint pinning'
  assert_contains "$matrix" 'packed e2e covers command execution, known_hosts rejection, and SHA-256 host-key fingerprint pinning'
  assert_contains "$matrix" 'opt-in SSH/SFTP e2e'
  assert_contains "$matrix" 'lower-level dependency-native channels remain intentionally outside'
  assert_contains "$repo_root/TODO.md" '\[x\] Expose Lua SFTP open flag constants'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_facade_contracts'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'vectis-lua-facade-contracts-ok'
  assert_contains "$repo_root/TODO.md" '\[x\] Extend Vectis-owned Lua `nil, err` objects with C error source metadata'
  assert_contains "$repo_root/docs/lua-conventions.md" 'err\.source'
  assert_contains "$repo_root/docs/lua-conventions.md" 'err\.dependency_code'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERROR_SOURCE_VECTIS"'
  assert_contains "$repo_root/src/vectis_cli.c" 'error_source_string'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'error_source_string'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'source_code'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'canonical_source\.source == "curl"'
  assert_contains "$repo_root/lua/vectis/status.lua" 'err\.source = M\.error_source_string\(source\)'
  assert_contains "$repo_root/docs/lua-status.md" 'canonical `source` text derived from'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'ERROR_SOURCE_VECTIS'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'rest\.error_response'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'unserializable_error_response'
  assert_contains "$repo_root/lua/vectis/rest.lua" 'REST error response JSON encode failed'
  assert_contains "$repo_root/docs/lua-rest.md" 'minimal `500` JSON response'
  assert_contains "$repo_root/TODO.md" '\[x\] Normalize `vectis\.rest\.error_response` JSON encode failures'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'unsupported_json ='
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'outbound_json\.transport_ok == false'
  assert_contains "$repo_root/lua/vectis/rest.lua" 'function client\.post'
  assert_contains "$repo_root/lua/vectis/rest.lua" 'request_error\(err\)'
  assert_contains "$repo_root/docs/lua-rest.md" 'If request JSON cannot be encoded'
  assert_contains "$repo_root/TODO.md" '\[x\] Normalize outbound `vectis\.rest\.client` JSON encode failures'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'ERROR_SOURCE_LONEJSON'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERR_NOMEM"'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERR_STATE"'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERR_CONFLICT"'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERR_TIMEOUT"'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.ERR_NOMEM'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.ERR_STATE'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.ERR_CONFLICT'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.ERR_TIMEOUT'
  assert_contains "$repo_root/tests/lua/dsv.cmake" 'stop_err\.status == vectis\.ERR_STATE'
  assert_contains "$repo_root/lua/vectis/dsv.lua" 'vectis_error\("ERR_STATE"'
  assert_contains "$lua_index" 'Dependency-Native Modules'
  assert_contains "$lua_index" 'Vectis Workflow Modules'
  assert_contains "$lua_index" '\[Lua facade conventions\]\(lua-conventions\.md\)'
  assert_contains "$lua_index" '\[Lua coverage matrix\]\(lua-coverage-matrix\.md\)'
  assert_contains "$lua_index" '\[SUS and audio contract\]\(lua-sus-audio-contract\.md\)'
  assert_contains "$lua_index" '## Framework Model'
  assert_contains "$lua_index" 'Dependency-native modules expose bundled libraries directly'
  assert_contains "$lua_index" '`require\("vectis"\)\.libs` is an alias table'
  assert_contains "$lua_index" 'Do not add a Vectis-owned helper solely'
  assert_contains "$lua_index" 'mirror a dependency'
  assert_contains "$repo_root/TODO.md" '\[x\] For each dependency-native Lua facade, add a Vectis-owned helper only where it reduces real service workflow friction'
  assert_contains "$repo_root/TODO.md" '\[x\] Keep the Lua framework model aligned with the C SDK model'
  assert_contains "$repo_root/TODO.md" '\[x\] Keep CAI itself as the primary SDK for OpenAI mechanics'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Expected runtime failures return structured values'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Programmer misuse raises a Lua error'
  assert_contains "$repo_root/docs/lua-conventions.md" '## Naming And Options'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Timeout and duration fields use unit-suffixed names'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Callback-owned request/event tables contain copied scalar values'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Do not describe a materialized or spooled path as streaming'
  assert_contains "$repo_root/TODO.md" '\[x\] Mirror the C naming, source, error, timeout, ownership, and cleanup convention in Lua'
  assert_contains "$lua_index" '`lockdc`'
  assert_contains "$lua_index" '`lonejson`'
  assert_contains "$lua_index" '\[Lua pslog\]\(lua-pslog\.md\)'
  assert_contains "$lua_index" '\[Lua lql\]\(lua-lql\.md\)'
  assert_contains "$lua_index" '\[Lua CAI\]\(lua-cai\.md\)'
  assert_contains "$repo_root/docs/api.md" '\[Lua CAI\]\(lua-cai\.md\)'
  assert_contains "$repo_root/docs/lua-cai.md" 'dependency-native CAI Lua module'
  assert_contains "$repo_root/docs/lua-cai.md" 'The pinned CAI dependency is `0\.4\.0`'
  assert_contains "$repo_root/docs/lua-cai.md" '`vectis\.cai` is a service DX'
  assert_contains "$repo_root/docs/lua-cai.md" 'not a second AI SDK'
  assert_contains "$repo_root/docs/lua-cai.md" 'The C adapters preserve true streaming'
  assert_contains "$repo_root/TODO.md" '\[x\] Track CAI as a dependency once its C SDK surface stabilizes'
  assert_contains "$matrix" 'Dependency-native CAI 0\.4\.0 module is preloaded'
  assert_contains "$matrix" '`vectis\.cai` covers normalized service config'
  assert_contains "$lua_index" '\[Lua libmdf\]\(lua-libmdf\.md\)'
  assert_contains "$lua_index" '\[Lua softline\]\(lua-softline\.md\)'
  assert_contains "$repo_root/docs/api.md" '\[Lua libmdf\]\(lua-libmdf\.md\)'
  assert_contains "$repo_root/docs/api.md" '\[Lua softline\]\(lua-softline\.md\)'
  assert_contains "$repo_root/docs/lua-libmdf.md" 'dependency-native libmdf Lua module'
  assert_contains "$repo_root/docs/lua-libmdf.md" 'libmdf\.render_stream'
  assert_contains "$repo_root/docs/lua-softline.md" 'dependency-native softline Lua module'
  assert_contains "$repo_root/docs/lua-softline.md" 'softline\.new'
  assert_contains "$matrix" 'documented in `docs/lua-libmdf\.md`'
  assert_contains "$matrix" 'documented in `docs/lua-softline\.md`'
  assert_contains "$lua_index" '`curl`'
  assert_contains "$lua_index" '`openssl`'
  assert_contains "$lua_index" '`zlib`'
  assert_contains "$lua_index" '`opcua`'
  assert_contains "$lua_index" '`audio`'
  assert_contains "$lua_index" '`sus`'
  assert_contains "$lua_index" '`vectis\.auth`'
  assert_contains "$lua_index" '`vectis\.audio_worker`'
  assert_contains "$lua_index" '`vectis\.cai_worker`'
  assert_contains "$lua_index" '`vectis\.sus_worker`'
  assert_contains "$lua_index" '`vectis\.cert`'
  assert_contains "$lua_index" '`vectis\.curl_worker`'
  assert_contains "$lua_index" '`vectis\.dsv`'
  assert_contains "$lua_index" '`vectis\.http`'
  assert_contains "$lua_index" '`vectis\.lockd`'
  assert_contains "$lua_index" '`vectis\.mqtt`'
  assert_contains "$lua_index" '`vectis\.server`'
  assert_contains "$lua_index" '`vectis\.status`'
  assert_contains "$lua_index" '`vectis\.smtp`'
  assert_contains "$lua_index" '`vectis\.ssh`'
  assert_contains "$lua_index" '`vectis\.terminal`'
  assert_contains "$lua_index" '`vectis\.webdav`'
  assert_contains "$lua_index" '`vectis\.xml`'
  assert_contains "$lua_index" '`vectis\.embedded`'
  assert_contains "$lua_index" 'same identity rule'
  assert_contains "$repo_root/TODO.md" '\[x\] Expand `require\("vectis"\)` into the full high-level framework facade'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.http == http'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.lockd == lockd'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'package\.loaded\["vectis\.http"\] == http'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'package\.loaded\["vectis\.lockd"\] == lockd'
  assert_contains "$lua_index" 'preloaded modules inside the embedded'
  assert_contains "$lua_index" 'vectis\.auth` re-exports the C-owned `vectis\.auth\.core` facade'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.auth"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.audio_worker"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.cai_worker"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.sus_worker"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.cert"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.curl_worker"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.embedded"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.server"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.ssh"'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "auth", "vectis\.auth"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "audio_worker", "vectis\.audio_worker"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "cai_worker", "vectis\.cai_worker"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "sus_worker", "vectis\.sus_worker"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "curl_worker", "vectis\.curl_worker"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "server", "vectis\.server"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "embedded", "vectis\.embedded"\)'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.auth"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.audio_worker"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.cai_worker"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.sus_worker"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.curl_worker"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.auth == loaded\["vectis\.auth"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.audio_worker == loaded\["vectis\.audio_worker"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.cai_worker == loaded\["vectis\.cai_worker"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.sus_worker == loaded\["vectis\.sus_worker"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis\.audio_worker"\]\.vox_request'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis\.audio_worker"\]\.decode_vox_segment'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.curl_worker == loaded\["vectis\.curl_worker"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.smtp == loaded\["vectis\.smtp"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.lockd == loaded\["vectis\.lockd"\]'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'vectis\.libs\.'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.auth"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.audio_worker"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.cai_worker"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.sus_worker"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vox_request'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'decode_vox_segment'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.curl_worker"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.auth == auth'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_pushliteral\(lua, "vectis\.smtp"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_setfield\(lua, -2, "smtp"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.rest", vectis_rest_lua_init'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_setfield\(lua, -2, "rest"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.terminal", vectis_terminal_lua_init'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_setfield\(lua, -2, "terminal"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.rest == rest'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.terminal == terminal'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.smtp == smtp'
  assert_contains "$lua_index" '\[Lua curl\]\(lua-curl\.md\)'
  assert_contains "$lua_index" '\[Lua OpenSSL\]\(lua-openssl\.md\)'
  assert_contains "$lua_index" '\[Lua zlib\]\(lua-zlib\.md\)'
  assert_contains "$lua_index" '\[Lua audio\]\(lua-audio\.md\)'
  assert_contains "$lua_index" '\[Lua SUS\]\(lua-sus\.md\)'
  assert_contains "$lua_index" '\[Lua auth\]\(lua-auth\.md\)'
  assert_contains "$lua_index" '\[Lua certificates\]\(lua-certs\.md\)'
  assert_contains "$lua_index" '\[Lua embedded assets\]\(lua-embedded\.md\)'
  assert_contains "$lua_index" '\[Lua DSV\]\(lua-dsv\.md\)'
  assert_contains "$lua_index" '\[Lua HTTP\]\(lua-http\.md\)'
  assert_contains "$repo_root/docs/lua-embedded.md" 'require\("vectis\.embedded"\)'
  assert_contains "$repo_root/docs/lua-embedded.md" 'structured Vectis errors'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'type\(embedded\.read\) == "function"'
  assert_contains "$repo_root/TODO.md" '\[x\] Add user-facing docs and smoke coverage for the direct `vectis\.embedded`'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.head'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.options'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.options_json'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.download_file'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.upload_file'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.sftp_download_file'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function M\.sftp_upload_file'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function client\.options_json'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function client\.download_file'
  assert_contains "$repo_root/lua/vectis/http.lua" 'function client\.upload_file'
  assert_contains "$repo_root/docs/lua-http.md" '`delete`,'
  assert_contains "$repo_root/docs/lua-http.md" '`head`, and `options` set the HTTP method'
  assert_contains "$repo_root/docs/lua-http.md" '`options_json` set the HTTP method'
  assert_contains "$repo_root/docs/lua-http.md" 'download_file\(url, path'
  assert_contains "$matrix" 'file upload/download presets'
  assert_contains "$matrix" 'JSON helpers including OPTIONS'
  assert_contains "$repo_root/tests/lua/http.cmake" 'vectis\.http\.head'
  assert_contains "$repo_root/tests/lua/http.cmake" 'vectis\.http\.options'
  assert_contains "$repo_root/tests/lua/http.cmake" 'vectis\.http\.options_json'
  assert_contains "$repo_root/tests/lua/http.cmake" 'vectis\.http\.download_file'
  assert_contains "$repo_root/tests/lua/http.cmake" 'file_client\.download_file'
  assert_contains "$repo_root/tests/lua/http.cmake" 'authenticated_client\.head'
  assert_contains "$repo_root/tests/lua/http.cmake" 'authenticated_client\.options'
  assert_contains "$repo_root/tests/lua/http.cmake" 'authenticated_client\.options_json'
  assert_contains "$repo_root/lua/vectis/rest.lua" 'function client\.head'
  assert_contains "$repo_root/lua/vectis/rest.lua" 'function client\.options'
  assert_contains "$repo_root/docs/lua-rest.md" '`get`, `post`, `put`, `patch`, `delete`, `head`, and `options`'
  assert_contains "$repo_root/tests/lua/http.cmake" 'rest_client\.head'
  assert_contains "$repo_root/tests/lua/http.cmake" 'rest_client\.options'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'smoke_rest_client\.head'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'smoke_rest_client\.options'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'smoke_http\.options_json'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'smoke_http\.download_file'
  assert_contains "$repo_root/TODO.md" '\[x\] Add `head` to the `vectis\.rest\.client`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add `options` to `vectis\.http`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add `options_json` to `vectis\.http`'
  assert_contains "$repo_root/TODO.md" '\[x\] Add richer Lua HTTP file-backed response presets'
  assert_contains "$lua_index" '\[Lua lockd\]\(lua-lockd\.md\)'
  assert_contains "$lua_index" '\[Lua logging\]\(lua-log\.md\)'
  assert_contains "$lua_index" '\[Lua MQTT\]\(lua-mqtt\.md\)'
  assert_contains "$lua_index" '\[Lua REST\]\(lua-rest\.md\)'
  assert_contains "$lua_index" '\[Lua server\]\(lua-server\.md\)'
  assert_contains "$lua_index" '\[Lua status\]\(lua-status\.md\)'
  assert_contains "$lua_index" '\[Lua SMTP\]\(lua-smtp\.md\)'
  assert_contains "$lua_index" '\[Lua SSH\]\(lua-ssh\.md\)'
  assert_contains "$lua_index" '\[Lua terminal\]\(lua-terminal\.md\)'
  assert_contains "$lua_index" '\[Lua WebDAV\]\(lua-webdav\.md\)'
  assert_contains "$lua_index" '\[Lua XML\]\(lua-xml\.md\)'
  assert_contains "$lua_index" 'The C SDK artifacts intentionally do not ship the embedded Lua runtime'
  assert_contains "$matrix" 'docs/lua-audio\.md'
  assert_contains "$matrix" 'docs/lua-sus\.md'
  assert_contains "$repo_root/docs/lua-audio.md" 'audio\.decoder\.open_reader'
  assert_contains "$repo_root/docs/lua-audio.md" 'audio\.capture\.open_default'
  assert_contains "$repo_root/docs/lua-audio.md" 'audio\.playback\.open_default'
  assert_contains "$repo_root/docs/lua-sus.md" 'sus\.open_cached'
  assert_contains "$repo_root/docs/lua-sus.md" 'model:reset_transcript_spacing'
  assert_contains "$repo_root/docs/lua-sus-audio-contract.md" 'docs/lua-audio\.md'
  assert_contains "$repo_root/docs/lua-sus-audio-contract.md" 'docs/lua-sus\.md'
  assert_contains "$repo_root/TODO.md" '\[x\] Ensure the `vectis` Lua facade exposes structured status/error objects consistently'
  assert_contains "$repo_root/include/vectis/webdav.h" 'const char \*root_dir'
  assert_contains "$repo_root/src/vectis_cli.c" '"root_dir"'
  assert_contains "$repo_root/src/vectis_webdav.c" 'vectis_webdav_direct_root'
  assert_contains "$repo_root/docs/lua-webdav.md" 'root_dir` to serve a direct'
  assert_contains "$repo_root/tests/lua/webdav_server.cmake" 'root_dir = root_dir'
  assert_contains "$matrix" 'direct mutable disk `root_dir` mounts'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.markdown'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.markdown_stream'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.editor'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'libs ='
  assert_contains "$repo_root/docs/lua-terminal.md" 'terminal\.markdown_stream'
  assert_contains "$repo_root/docs/lua-terminal.md" 'vectis\.terminal\.libs\.libmdf'
  assert_contains "$repo_root/examples/lua/terminal_tools.lua" 'terminal\.markdown'
  assert_contains "$repo_root/examples/lua/terminal_tools.lua" 'terminal\.editor'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_terminal_tools'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'terminal_tools\.lua'
  assert_contains "$matrix" '`vectis\.terminal` covers Markdown render'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.new'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.log_error'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.from_env'
  assert_contains "$repo_root/lua/vectis/log.lua" 'native = pslog'
  assert_contains "$repo_root/docs/lua-log.md" 'log\.log_error'
  assert_contains "$repo_root/docs/lua-log.md" 'log\.from_env'
  assert_contains "$repo_root/docs/lua-log.md" 'vectis\.log\.native'
  assert_contains "$repo_root/examples/lua/logging.lua" 'log\.log_error'
  assert_contains "$repo_root/examples/lua/logging.lua" 'log\.from_env'
  assert_contains "$repo_root/docs/lua-pslog.md" 'pslog\.new_json'
  assert_contains "$repo_root/docs/lua-pslog.md" 'pslog\.from_env'
  assert_contains "$repo_root/docs/lua-lql.md" 'client:selector_parse'
  assert_contains "$repo_root/docs/lua-lql.md" 'client:apply_string_spooled'
  assert_contains "$repo_root/docs/lua-lql.md" 'Do not add a Vectis-owned helper'
  assert_contains "$matrix" 'dep:lql.*\\| n/a \\|.*no Vectis-owned helper is planned'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'require\("lql"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'lql\.new'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_logging'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'logging\.lua'
  assert_contains "$matrix" '`vectis\.log` adds JSON logger defaults'
  assert_contains "$matrix" 'workflow:logging'
  assert_contains "$matrix" 'logger_disabled'
  assert_contains "$repo_root/docs/lua-server.md" 'Managed app-owned services inherit the server/app logger'
  assert_contains "$repo_root/include/vectis/auth.h" 'vectis_auth_basic_authorization'
  assert_contains "$repo_root/src/vectis_auth.c" 'EVP_EncodeBlock'
  assert_contains "$repo_root/src/vectis_cli.c" 'basic_authorization'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "vectis\.auth\.core"'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_lua_module'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_auth_lua_init'
  assert_contains "$repo_root/lua/vectis/auth.lua" 'function M\.browser_flow'
  assert_contains "$repo_root/lua/vectis/auth.lua" 'function browser_flow:mount'
  assert_contains "$repo_root/lua/vectis/auth.lua" 'function browser_flow:webdav_authorization'
  assert_contains "$repo_root/vectis.rockspec.in" '\["vectis\.auth"\] = "lua/vectis/auth\.lua"'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'stub\("vectis\.auth\.core"'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'vectis\.auth\.browser_flow'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'local expected_modules ='
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'local dependency_modules ='
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'vectis\.libs\[name\] == package\.loaded\[name\]'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'vectis\.libs\.curl == package\.loaded\.curl'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" '"vectis\.webdav"'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'webdav\.propfind'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'mqtt\.publish'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'smtp\.send'
  assert_contains "$repo_root/scripts/test_lua_rock.sh" 'terminal\.markdown'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.auth\.basic_authorization'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'vectis\.auth\.browser_flow'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'vectis\.auth\.basic_authorization'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'vectis\.auth\.browser_flow'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'require\("vectis\.rest"\)'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:auth_json'
  assert_contains "$repo_root/docs/lua-rest.md" 'REST does not provide lockd-specific route presets'
  assert_contains "$repo_root/examples/lua/lockd_state.lua" 'vectis\.lockd\.save_json'
  assert_contains "$repo_root/examples/lua/lockd_queue.lua" 'vectis\.lockd\.with_dequeued_json'
  assert_contains "$repo_root/TODO.md" '\[x\] Extend the REST DX with lockd operation presets only where examples show repeated service boilerplate'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:json'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'rest\.client'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'require\("vectis\.rest"\)'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'server:json'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'rest\.client'
  assert_contains "$repo_root/docs/lua-auth.md" 'basic_authorization'
  assert_contains "$repo_root/docs/lua-auth.md" 'browser_flow'
  assert_contains "$matrix" 'Basic Authorization formatting'
  assert_contains "$matrix" 'vectis\.auth\.browser_flow'
  assert_contains "$repo_root/TODO.md" 'dependency-native zlib Lua facade'
  assert_contains "$repo_root/src/vectis_cli.c" 'luaopen_zlib'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "zlib"'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("zlib"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'zlib\.gzip'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'zlib\.gzip_file'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a top-level `vectis\.libs` namespace'
  assert_contains "$repo_root/TODO.md" '\[x\] Keep `vectis\.libs` coverage current'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a pure Lua top-level `vectis` facade'
  assert_contains "$repo_root/TODO.md" '\[x\] Publish a separate `vectis` Lua rock'
  assert_contains "$repo_root/TODO.md" 'C binary SDK artifacts'
  assert_contains "$repo_root/Makefile" 'lua-rock:'
  assert_contains "$repo_root/Makefile" 'release-lua-artifacts:'
  assert_contains "$repo_root/Makefile" 'release-matrix: package package-source release-lua-artifacts package-checksums verify-release-matrix'
  assert_contains "$repo_root/Makefile" '^format-check:'
  assert_not_contains "$repo_root/Makefile" '^release-pipeline:'
  assert_contains "$repo_root/Makefile" '^prerelease:'
  assert_contains "$repo_root/Makefile" '\$\(TIMED\) prerelease bash \./scripts/release_pipeline\.sh'
  assert_contains "$repo_root/Makefile" '\$\(TIMED\) release-worktree-clean bash \./scripts/require_clean_release_worktree\.sh'
  assert_contains "$repo_root/Makefile" '\$\(TIMED\) release-pipeline bash \./scripts/release_pipeline\.sh'
  assert_contains "$repo_root/scripts/release_pipeline.sh" 'format-check'
  assert_contains "$repo_root/scripts/release_pipeline.sh" 'release-matrix'
  assert_contains "$repo_root/Makefile" 'scripts/build_lua_rock\.sh'
  assert_contains "$repo_root/Makefile" 'scripts/stage_lua_rock_sources\.sh'
  assert_contains "$repo_root/vectis.rockspec.in" 'package = "vectis"'
  assert_contains "$repo_root/vectis.rockspec.in" '\["vectis"\] = "lua/vectis\.lua"'
  assert_contains "$repo_root/scripts/build_lua_rock.sh" 'build/luarocks'
  assert_contains "$repo_root/scripts/render_release_rockspec.sh" 'VECTIS_LUA_SOURCE_URL'
  assert_contains "$repo_root/scripts/stage_lua_rock_sources.sh" 'package_name="vectis-lua-\$version"'
  assert_contains "$repo_root/scripts/stage_lua_rock_sources.sh" 'archive="\$dist_dir/\$package_name\.tar\.gz"'
  assert_contains "$repo_root/scripts/stage_lua_rock_sources.sh" 'docs/lua\*\.md'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'source rock missing rendered rockspec'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'source rock nested archive missing vectis\.lua'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'check_lua_module_files'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'check_lua_docs'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'Lua source archive missing linked documentation'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'Lua source archive missing rockspec module file'
  assert_contains "$repo_root/scripts/validate_luarocks.sh" 'Lua source archive manifest missing rockspec module file'
  assert_contains "$repo_root/cmake/package_checksums.cmake" 'vectis-lua-\$\{VECTIS_VERSION\}\.tar\.gz'
  assert_contains "$repo_root/cmake/package_checksums.cmake" 'vectis-\$\{VECTIS_VERSION\}-1\.rockspec'
  assert_contains "$repo_root/cmake/package_checksums.cmake" 'vectis-\$\{VECTIS_VERSION\}-1\.src\.rock'
  assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'vectis-lua-"\$version"\.tar\.gz'
  assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'validate_luarocks\.sh'
  assert_contains "$repo_root/scripts/verify_release_privacy.sh" '\*\.rockspec'
  assert_contains "$repo_root/scripts/verify_release_privacy.sh" '\*\.rock\|\*\.src\.rock'
  assert_contains "$repo_root/vendor/kore/patches/0024-kore-vectis-webdav-methods.patch" 'req->method == HTTP_METHOD_PROPFIND'
  assert_contains "$repo_root/vendor/kore/patches/0024-kore-vectis-webdav-methods.patch" 'req->method == HTTP_METHOD_MKCOL'
  assert_contains "$repo_root/vendor/kore/patches/0024-kore-vectis-webdav-methods.patch" 'flags \|= HTTP_REQUEST_EXPECT_BODY'
  assert_contains "$lua_index" 'make lua-rock'
  assert_contains "$lua_index" 'vectis-lua-<version>\.tar\.gz'
  assert_contains "$repo_root/lua/vectis.lua" 'local status = require\("vectis\.status"\)'
  assert_contains "$repo_root/lua/vectis.lua" 'function M\.embedded_lockd_bundle_source'
  assert_contains "$repo_root/lua/vectis/version.lua" 'return "0\.0\.0"'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_top_level_module'
  assert_contains "$repo_root/tests/lua/top_level_module.cmake" 'vectis top-level Lua module ok'
  assert_contains "$lua_index" 'pure Lua top-level `vectis` module'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_push_libs_table'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_setfield\(lua, -2, "libs"\)'
  assert_contains "$lua_index" '`require\("vectis"\)\.libs`'
  assert_contains "$matrix" '`vectis\.libs` aliases `lockdc`, `lonejson`, `pslog`, `lql`, `cai`, `libmdf`, `softline`, `curl`, `opcua`, `openssl`, `zlib`, `audio`, and `sus`'
  for vectis_lib_alias in lockdc lonejson pslog lql cai libmdf softline curl opcua openssl zlib audio sus; do
    assert_contains "$repo_root/src/vectis_cli.c" "vectis_lua_set_required_module\\(lua, \"$vectis_lib_alias\", \"$vectis_lib_alias\"\\)"
    assert_contains "$repo_root/tests/lua/smoke.lua" "vectis\\.libs\\.$vectis_lib_alias == $vectis_lib_alias"
    assert_contains "$lua_index" "\`vectis\\.libs\\.$vectis_lib_alias\`"
    assert_contains "$matrix" "\`$vectis_lib_alias\`"
  done
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'require\("zlib"\)'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'zlib\.decompress'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'zlib\.decompress_file'
  assert_contains "$repo_root/docs/lua-zlib.md" 'file-backed'
  assert_contains "$repo_root/docs/lua-zlib.md" 'Do not add a Vectis-owned'
  assert_contains "$repo_root/docs/api.md" '\[Lua zlib\]\(lua-zlib\.md\)'
  assert_contains "$repo_root/examples/README.md" 'zlib string/file'
  assert_contains "$matrix" 'file-backed bounded transforms'
  assert_contains "$matrix" 'dep:zlib.*\\| n/a \\|.*no Vectis-owned compression helper is planned'
  assert_contains "$matrix" 'packed local data pipeline example coverage'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"zlib"'
}

write_fake_pack_runner_inputs() {
  root=$1
  target_id=$2

  mkdir -p "$root/share/vectis" "$root/lib/vectis/pack"
  printf '%s\n' 'fake pack runner archive' \
    >"$root/lib/vectis/pack/libvectis_pack_runner.a"
  cat >"$root/share/vectis/pack-runner-link-inputs.json" <<EOF
{
  "format": "vectis-pack-runner-link-inputs",
  "version": 1,
  "vectis_version": "0.0.0",
  "target_id": "$target_id",
  "runner_archive": "lib/vectis/pack/libvectis_pack_runner.a",
  "cmake_target": "vectis::pack_runner",
  "payload_sections": ["__VECTIS,__pack_header"]
}
EOF
}

assert_luarocks_artifact_rejected() {
  dist=$luarocks_dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=vectis-$version-x86_64-linux-musl.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
	mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/share/c.pkt.systems" \
	  "$root/share/doc/vectis" \
	  "$root/.luarocks"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	printf '%s\n' 'target_id=x86_64-linux-musl' >"$root/share/c.pkt.systems/manifest.txt"
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  write_fake_pack_runner_inputs "$root" x86_64-linux-musl
  printf '%s\n' 'must not ship' >"$root/.luarocks/bad.rock"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/verify.out" 2>"$dist/verify.err"; then
    echo "release artifact verifier accepted LuaRocks artifacts" >&2
    exit 1
  fi
  if ! grep -Fq "binary SDK contains LuaRocks artifacts" "$dist/verify.err"; then
    echo "release artifact verifier rejected LuaRocks fixture for the wrong reason" >&2
    cat "$dist/verify.err" >&2
    exit 1
  fi
}

assert_linux_release_target_payload_checked() {
  dist=$version_work/target-payload-dist
  version=0.0.0
  root_name=vectis-$version-aarch64-linux-gnu
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/share/c.pkt.systems" \
	  "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	printf '%s\n' 'target_id=aarch64-linux-gnu' >"$root/share/c.pkt.systems/manifest.txt"
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  write_fake_pack_runner_inputs "$root" aarch64-linux-gnu
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/target.out" 2>"$dist/target.err"; then
    echo "release artifact verifier accepted a mislabeled Linux SDK payload" >&2
    exit 1
  fi
  if ! grep -Fq "Linux binary SDK ELF machine target mismatch" "$dist/target.err"; then
    echo "release artifact verifier rejected mislabeled Linux SDK payload for the wrong reason" >&2
    cat "$dist/target.err" >&2
    exit 1
  fi
}

assert_linux_release_manifest_target_checked() {
  dist=$version_work/target-manifest-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$root/share/c.pkt.systems"
  printf '%s\n' 'target_id=x86_64-linux-gnu' >"$root/share/c.pkt.systems/manifest.txt"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/manifest.out" 2>"$dist/manifest.err"; then
    echo "release artifact verifier accepted a mislabeled c.pkt.systems target manifest" >&2
    exit 1
  fi
  if ! grep -Fq "binary SDK c.pkt.systems target manifest mismatch" "$dist/manifest.err"; then
    echo "release artifact verifier rejected mislabeled c.pkt.systems manifest for the wrong reason" >&2
    cat "$dist/manifest.err" >&2
    exit 1
  fi
}

assert_linux_release_binary_static_checked() {
  dist=$version_work/static-binary-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  -d:*/bin/vectis)
    printf '%s\n' ' 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]'
    ;;
  -l:*/bin/vectis)
    printf '%s\n' '  INTERP         0x0000000000000318 0x0000000000400318 0x0000000000400318'
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
	mkdir -p \
	  "$root/bin" \
	  "$root/include/vectis" \
	  "$root/lib/cmake/vectis" \
	  "$root/lib/pkgconfig" \
	  "$root/share/c.pkt.systems" \
	  "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
	printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
	printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
	printf '%s\n' 'target_id=x86_64-linux-musl' >"$root/share/c.pkt.systems/manifest.txt"
	printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  write_fake_pack_runner_inputs "$root" x86_64-linux-musl
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_RELEASE_BUILD_ROOT="$dist/no-build" \
       VECTIS_READELF="$dist/fakebin/readelf" \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/static.out" 2>"$dist/static.err"; then
    echo "release artifact verifier accepted a dynamically linked Linux vectis binary" >&2
    exit 1
  fi
  if ! grep -Fq "Linux vectis binary is dynamically linked" "$dist/static.err"; then
    echo "release artifact verifier rejected dynamic Linux vectis binary for the wrong reason" >&2
    cat "$dist/static.err" >&2
    exit 1
  fi
}

assert_linux_release_readelf_discovered_from_target_cache() {
  dist=$version_work/readelf-cache-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=$root_name.tar.gz
  root="$dist/$root_name"
  build_dir="$dist/build/x86_64-linux-musl-release"

  rm -rf "$dist"
  mkdir -p "$dist/fakebin" "$build_dir"
  cat >"$dist/fakebin/readelf" <<'EOF'
#!/bin/sh
case "$1:$2" in
  -h:*/bin/vectis|-h:*/lib/libvectis.a)
    cat <<'HEADER'
ELF Header:
  Class:                             ELF64
  Machine:                           Advanced Micro Devices X86-64
HEADER
    ;;
  -d:*/bin/vectis)
    exit 1
    ;;
  -l:*/bin/vectis)
    exit 1
    ;;
  *) exit 1 ;;
esac
EOF
  chmod +x "$dist/fakebin/readelf"
  printf '%s\n' "CMAKE_READELF:FILEPATH=$dist/fakebin/readelf" >"$build_dir/CMakeCache.txt"
  mkdir -p \
    "$root/bin" \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/share/c.pkt.systems" \
    "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '%s\n' 'fake archive' >"$root/lib/libvectis.a"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  printf '%s\n' 'target_id=x86_64-linux-musl' >"$root/share/c.pkt.systems/manifest.txt"
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  write_fake_pack_runner_inputs "$root" x86_64-linux-musl
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  VECTIS_RELEASE_BUILD_ROOT="$dist/build" \
    VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
    "$repo_root/scripts/verify_release_artifacts.sh" \
    >"$dist/readelf.out" 2>"$dist/readelf.err"
}

assert_linux_release_matrix_required() {
  dist=$version_work/matrix-dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-gnu
  artifact=$root_name.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p \
    "$root/bin" \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/share/doc/vectis"
  printf '#!/bin/sh\nexit 0\n' >"$root/bin/vectis"
  chmod +x "$root/bin/vectis"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1 \
       VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
       "$repo_root/scripts/verify_release_artifacts.sh" \
       >"$dist/matrix.out" 2>"$dist/matrix.err"; then
    echo "release artifact verifier accepted an incomplete Linux release matrix" >&2
    exit 1
  fi
  if ! grep -Fq "missing required Linux release artifact" "$dist/matrix.err"; then
    echo "release artifact verifier rejected incomplete Linux matrix for the wrong reason" >&2
    cat "$dist/matrix.err" >&2
    exit 1
  fi
}

if [ -f "$version_path" ]; then
  had_version=1
  saved_version=$(cat "$version_path")
fi

assert_contains "$repo_root/.gitignore" '^/VERSION$'
assert_contains "$repo_root/scripts/package.sh" 'cpkt-toolchains\.sh" ensure "\$target_id"'
assert_contains "$repo_root/scripts/package.sh" 'rm -rf "\$build_dir"'
assert_contains "$repo_root/scripts/package.sh" 'refusing to clean unexpected build dir'
assert_not_contains "$repo_root/scripts/package.sh" 'run_optional_target'
assert_contains "$repo_root/scripts/require_clean_release_worktree.sh" 'git -C "\$repo_root" diff --quiet -- \.'
assert_contains "$repo_root/scripts/require_clean_release_worktree.sh" 'git -C "\$repo_root" ls-files --others --exclude-standard'
assert_contains "$repo_root/scripts/check_format.sh" 'clang-format --dry-run --Werror'
assert_contains "$repo_root/CMakePresets.json" 'x86_64-linux-gnu-afl\.cmake'
assert_contains "$repo_root/cmake/toolchains/x86_64-linux-gnu-afl.cmake" 'scripts/cpkt-toolchains\.sh'
assert_contains "$repo_root/cmake/toolchains/x86_64-linux-gnu-afl.cmake" 'discover x86_64-linux-gnu'
assert_contains "$repo_root/cmake/toolchains/x86_64-linux-gnu-afl.cmake" 'scripts/cpkt-aflpp\.sh'
assert_contains "$repo_root/tests/fuzz/CMakeLists.txt" 'VECTIS_AFL_FUZZER'
assert_contains "$repo_root/Makefile" 'scripts/configure_fuzz\.sh \$\(FUZZ_PRESET\)'
assert_contains "$repo_root/scripts/configure_fuzz.sh" 'x86_64-linux-gnu-afl\.cmake'
assert_contains "$repo_root/scripts/configure_fuzz.sh" 'cpkt-afl-gcc'
assert_contains "$repo_root/scripts/test_fuzz_smoke.sh" 'afl_showmap'
assert_contains "$repo_root/CMakeLists.txt" 'generated/pkgconfig/vectis\.pc'
assert_contains "$repo_root/CMakeLists.txt" 'pkgconfig"\)'
assert_contains "$repo_root/cmake/vectis.pc.in" '^Name: vectis$'
assert_contains "$repo_root/cmake/package_archive.cmake" 'share/c\.pkt\.systems'
assert_contains "$repo_root/cmake/package_archive.cmake" 'CMAKE_INSTALL_NAME_TOOL'
assert_contains "$repo_root/cmake/package_archive.cmake" '@rpath/\$\{vectis_darwin_dep_name\}'
assert_contains "$repo_root/cmake/package_archive.cmake" 'find_dependency\(CpktOpcUa CONFIG REQUIRED\)'
assert_contains "$repo_root/cmake/package_archive.cmake" 'find_dependency\(CpktLuaRuntime CONFIG REQUIRED\)'
assert_contains "$repo_root/cmake/package_archive.cmake" 'find_dependency\(CpktSus CONFIG REQUIRED\)'
assert_contains "$repo_root/cmake/package_archive.cmake" 'find_dependency\(CpktAudio CONFIG REQUIRED\)'
assert_contains "$repo_root/cmake/package_archive.cmake" 'find_package\(liblql CONFIG REQUIRED'
assert_contains "$repo_root/cmake/package_archive.cmake" 'cpkt::opcua'
assert_contains "$repo_root/cmake/package_archive.cmake" 'cpkt::sus'
assert_contains "$repo_root/cmake/package_archive.cmake" 'cpkt::audio'
assert_contains "$repo_root/cmake/package_archive.cmake" 'liblql::lql_static'
assert_contains "$repo_root/CMakeLists.txt" 'vectis_pack_runner'
assert_contains "$repo_root/CMakeLists.txt" 'pack-runner-link-inputs\.json'
assert_contains "$repo_root/CMakeLists.txt" 'CMAKE_INSTALL_LIBDIR.*/vectis/pack'
assert_contains "$repo_root/cmake/pack-runner-link-inputs.json.in" \
  '"format": "vectis-pack-runner-link-inputs"'
assert_contains "$repo_root/cmake/pack-runner-link-inputs.json.in" \
  '"runner_archive": "lib/vectis/pack/libvectis_pack_runner.a"'
assert_contains "$repo_root/cmake/pack-runner-link-inputs.json.in" \
  '__VECTIS,__pack_header'
assert_contains "$repo_root/cmake/vectisConfig.cmake.in" 'vectis::pack_runner'
assert_contains "$repo_root/cmake/package_archive.cmake" 'vectis::pack_runner'
assert_contains "$repo_root/tests/install/CMakeLists.txt" 'cpkt::opcua cpkt::sus cpkt::audio'
assert_contains "$repo_root/tests/install/CMakeLists.txt" 'liblql::lql_static'
assert_contains "$repo_root/tests/install/CMakeLists.txt" 'vectis::pack_runner'
assert_contains "$repo_root/cmake/package_darwin_smoke_bundle.cmake" '@executable_path/\.\./lib'
assert_contains "$repo_root/scripts/verify_darwin_pack_signature.sh" \
  '\-\-verify \-\-strict \-\-verbose=4'
assert_contains "$repo_root/scripts/verify_darwin_pack_signature.sh" \
  '\-\-assess \-\-type execute'
assert_contains "$repo_root/scripts/verify_darwin_pack_signature.sh" \
  '\-\-require-spctl'
assert_contains "$repo_root/scripts/verify_darwin_pack_signature.sh" \
  'binary changed during %s'
assert_contains "$repo_root/scripts/test_darwin_pack_signature_verifier.sh" \
  'Darwin signature verifier accepted codesign failure'
assert_contains "$repo_root/scripts/test_darwin_pack_signature_verifier.sh" \
  'Darwin signature verifier accepted codesign verification mutation'
assert_contains "$repo_root/scripts/test_darwin_pack_signature_verifier.sh" \
  'Darwin signature verifier accepted spctl assessment mutation'
assert_contains "$repo_root/Makefile" '^test-darwin-pack-signature:'
assert_contains "$repo_root/Makefile" \
  'scripts/test_darwin_pack_signature_verifier\.sh'
assert_contains "$repo_root/Makefile" '^test-darwin-smoke-bundle:'
assert_contains "$repo_root/Makefile" \
  'scripts/test_darwin_smoke_bundle_verifier\.sh'
assert_contains "$repo_root/Makefile" \
  'test-darwin-smoke-bundle bash \./scripts/test_darwin_smoke_bundle_verifier\.sh'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'scripts/verify_darwin_pack_signature\.sh --binary <path>'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'scripts/verify_darwin_smoke_bundle\.sh'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'codesign --verify --strict --verbose=4'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'spctl --assess --type execute'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'fails if either verification command mutates the file'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'darwin-mach-o-pack-spec\.md'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  '__VECTIS,__pack_header'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  '__VECTIS,__pack_script'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  '__VECTIS,__pack_assets'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'Darwin should use a relink-based pack backend'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'pack-runner link inputs'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'No command may mutate the output after step 5'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'scripts/discover_target_tools\.sh'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'self-contained like Linux'
assert_contains "$repo_root/src/vectis_cli.c" \
  'hardened-runtime, --timestamp, and --entitlements'
assert_contains "$repo_root/src/vectis_cli.c" \
  'entitlements path is not a regular file'
assert_contains "$repo_root/src/vectis_cli.c" \
  'unsupported pack target'
assert_contains "$repo_root/src/vectis_cli.c" \
  'Darwin pack requires pack-runner link inputs'
assert_contains "$repo_root/src/vectis_cli.c" \
  'share/vectis/pack-runner-link-inputs\.json'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_pack_collect\(vectis_pack_payload \*payload'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_pack_write_elf\(const char \*output_path'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_pack_write_macho\(const char \*output_path'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis-pack-macho-sections\.c'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_pack_run_command'
assert_contains "$repo_root/src/vectis_cli.c" \
  'find_package\(vectis CONFIG REQUIRED\)'
assert_contains "$repo_root/src/vectis_cli.c" \
  'Mach-O pack CMake configure'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_lua_load_embedded_from_macho'
assert_contains "$repo_root/src/vectis_cli.c" \
  'getsectiondata'
assert_contains "$repo_root/src/vectis_cli.c" \
  '__pack_header'
assert_contains "$repo_root/src/vectis_cli.c" \
  'vectis_pack_verify_macho_artifact'
assert_contains "$repo_root/src/vectis_cli.c" \
  'Mach-O pack codesign verify'
assert_contains "$repo_root/src/vectis_cli.c" \
  'Mach-O pack artifact inspection'
assert_contains "$repo_root/src/vectis_cli.c" \
  '\-\-pack-sdk-root'
assert_contains "$repo_root/src/vectis_cli.c" \
  '\-\-work-dir'
assert_contains "$repo_root/src/vectis_cli.c" \
  '\-\-pack-toolchain-file'
assert_contains "$repo_root/src/vectis_cli.c" \
  'pack SDK manifest does not match target'
assert_contains "$repo_root/src/vectis_cli.c" \
  'validating pack-runner link inputs'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack signing option without signing mode'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack missing entitlements'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'vectis -a pack --target native failed'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target created an output artifact without link inputs'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target with link inputs created an output artifact before Mach-O relink support'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target did not write the Mach-O section source'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target did not write the Mach-O relink CMake project'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'Vectis Mach-O pack sections must be compiled for Darwin'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'vectis::pack_runner'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target with fake relink/sign tools'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'codesign:--verify --strict --verbose=4'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack Darwin target with mismatched link inputs created an output artifact'
assert_contains "$repo_root/tests/lua/pack.cmake" \
  'pack unknown target created an output artifact'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'vectis -a pack --target arm64-apple-darwin'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'Darwin pack requires pack-runner link'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  '\-\-pack-sdk-root <root>'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  '\-\-work-dir <dir>'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'installed-SDK CMake relink project'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'Darwin-capable `otool -hv`'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'Darwin startup uses the system Mach-O section API'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'lib/vectis/pack/libvectis_pack_runner.a'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  'vectis::pack_runner'
assert_contains "$repo_root/docs/darwin-mach-o-pack-spec.md" \
  'readable regular file'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Split `vectis -a pack` into shared `vectis_pack_collect`'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Add `vectis -a pack --pack-sdk-root <root>` validation'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Add the first `vectis_pack_write_macho` backend slice'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Extend `vectis_pack_write_macho` to generate an installed-SDK CMake relink project'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Split embedded payload startup into platform-specific locators'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Add Darwin packing flags for automatic codesigning'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Ensure Darwin packing never mutates the executable after codesigning'
assert_contains "$repo_root/TODO.md" \
  'docs/darwin-mach-o-pack-spec\.md'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'macos-15'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'macos-latest'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'gh release download'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'curl -fsSL "\$SMOKE_ZIP_URL"'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'shasum -a 256 "\$VECTIS_SMOKE_ZIP"'
assert_contains "$repo_root/.github/workflows/darwin-arm64-smoke.yml" \
  'scripts/verify_darwin_smoke_bundle\.sh'
assert_contains "$repo_root/scripts/verify_darwin_smoke_bundle.sh" \
  'Mach-O \.\*arm64'
assert_contains "$repo_root/scripts/verify_darwin_smoke_bundle.sh" \
  '\-\-verify \-\-strict \-\-verbose=4'
assert_contains "$repo_root/scripts/verify_darwin_smoke_bundle.sh" \
  '\-\-assess \-\-type execute'
assert_contains "$repo_root/scripts/verify_darwin_smoke_bundle.sh" \
  'binary changed during %s'
assert_contains "$repo_root/scripts/verify_darwin_smoke_bundle.sh" \
  'run-smoke\.sh'
assert_contains "$repo_root/scripts/test_darwin_smoke_bundle_verifier.sh" \
  'Darwin smoke bundle verifier accepted codesign mutation'
assert_contains "$repo_root/scripts/test_darwin_smoke_bundle_verifier.sh" \
  'Darwin smoke bundle verifier accepted spctl mutation'
assert_contains "$repo_root/scripts/test_darwin_smoke_bundle_verifier.sh" \
  'Darwin smoke bundle verifier accepted missing required spctl'
assert_contains "$repo_root/docs/pack-platform-operability.md" \
  '\.github/workflows/darwin-arm64-smoke\.yml'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Add verification commands/tests for packed Darwin binaries using `codesign --verify --strict --verbose=4`'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Add a GitHub Actions Darwin arm64 verification workflow using hosted `macos-15`/`macos-latest` runners'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'ubuntu-24\.04'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'pull_request:'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'push:'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'VECTIS_REQUIRE_LINUX_RELEASE_MATRIX: "1"'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  '~/.cache/c\.pkt\.systems/deps'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  '~/.cache/c\.pkt\.systems/toolchains'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'luarocks'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'make release-matrix'
assert_contains "$repo_root/.github/workflows/linux-release-matrix.yml" \
  'actions/upload-artifact@v4'
assert_contains "$repo_root/docs/ci-release-matrix.md" \
  'make release-matrix'
assert_contains "$repo_root/docs/ci-release-matrix.md" \
  'VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1'
assert_contains "$repo_root/docs/ci-release-matrix.md" \
  'local lifecycle release flow'
assert_contains "$repo_root/TODO.md" \
  '\[x\] Verify musl and cross targets against the full release matrix in CI'
assert_contains "$repo_root/scripts/verify_installed_sdk.sh" 'pkg-config --static --cflags --libs vectis'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK missing pkg-config metadata'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains dependency source tree'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains LuaRocks artifacts'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'VECTIS_REQUIRE_LINUX_RELEASE_MATRIX'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'missing required Linux release artifact'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'Linux binary SDK missing executable vectis binary'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK c\.pkt\.systems target manifest mismatch'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'discover_target_tools\.sh'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'VECTIS_RELEASE_BUILD_ROOT'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'Linux binary SDK ELF machine target mismatch'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'Linux static libvectis archive'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK missing pack-runner link input manifest'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'lib/vectis/pack/libvectis_pack_runner.a'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'verify_linux_vectis_binary_static'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'Linux vectis binary is dynamically linked'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'Linux vectis binary has an ELF interpreter'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'verify_linux_sdk_consumer_build'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'CMAKE_TOOLCHAIN_FILE="\$toolchain"'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'VECTIS_EXTERNAL_ROOT="\$root"'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'VECTIS_CONSUMER_LINK="\$link_mode"'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'verify_vectis_lua_preloads\.sh'
assert_contains "$repo_root/scripts/package.sh" 'verify_vectis_lua_preloads\.sh'
assert_contains "$repo_root/scripts/deps.sh" 'patch_lockdc_lua_source'
assert_contains "$repo_root/scripts/deps.sh" 'lcdc_opt_version_field'
assert_contains "$repo_root/scripts/deps.sh" 'lockdc_lua_patch=lc-version-if-version-helper'
assert_contains "$repo_root/Makefile" '^verify-release-matrix:'
assert_contains "$repo_root/Makefile" 'VECTIS_REQUIRE_LINUX_RELEASE_MATRIX=1 bash \./scripts/package-verify\.sh'
assert_contains "$repo_root/TODO.md" '\[x\] Add release verification for GNU and musl deliverables'
assert_contains "$repo_root/TODO.md" '\[x\] Statically preload every bundled dependency-native Lua facade'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'LUA_PATH="/__vectis_no_lua_path__/\?\.lua"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"lockdc"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"lonejson"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"curl"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"openssl"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.smtp"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"opcua"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"audio"'
assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"sus"'
assert_contains "$repo_root/CMakeLists.txt" 'target_link_options\(vectis_bin PRIVATE -static -no-pie\)'
assert_not_contains "$repo_root/CMakeLists.txt" 'set_property\(TARGET lockdc::static PROPERTY'
assert_contains "$repo_root/scripts/verify_release_privacy.sh" 'Linux vectis binary is dynamically linked'
assert_contains "$repo_root/scripts/verify_release_privacy.sh" 'Linux vectis binary has an ELF interpreter'
assert_contains "$repo_root/scripts/verify_release_privacy.sh" 'Darwin vectis binary depends on a non-system dylib'
assert_contains "$repo_root/TODO.md" 'codex review -c model=gpt-5\.6-sol -c model_reasoning_effort=medium --base <release-branch>'
assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local"'
assert_contains "$repo_root/CMakeLists.txt" 'target_compile_options\(\$\{target\} PRIVATE'
assert_contains "$repo_root/CMakeLists.txt" 'Werror'
assert_contains "$repo_root/CMakeLists.txt" '_DARWIN_C_SOURCE'
assert_contains "$repo_root/CMakeLists.txt" 'libpid0_enabled "0"'
assert_contains "$repo_root/scripts/deps.sh" 'libpid0_enabled=\$pid0_enabled'
assert_contains "$repo_root/examples/CMakeLists.txt" 'vectis_example_dependency_escape'
assert_contains "$repo_root/examples/CMakeLists.txt" 'dependency/dependency_escape_hatches\.c'
assert_contains "$repo_root/examples/README.md" '`dependency/`'
assert_contains "$repo_root/examples/CMakeLists.txt" 'target_compile_options\(\$\{target_name\} PRIVATE'
assert_contains "$repo_root/examples/CMakeLists.txt" 'Werror'
assert_no_landed_test_assets
assert_action_surface_contract
assert_lua_example_dx_contract
assert_concurrency_mailbox_contract
assert_kore_lonejson_contract
assert_kore_static_runtime_contract
assert_acme_lifecycle_contract
assert_lockdc_lua_runtime_contract
assert_lql_lua_runtime_contract
assert_opcua_lua_runtime_contract
assert_audio_sus_lua_runtime_contract
assert_lua_coverage_matrix_contract
assert_luarocks_artifact_rejected
assert_linux_release_target_payload_checked
assert_linux_release_manifest_target_checked
assert_linux_release_binary_static_checked
assert_linux_release_readelf_discovered_from_target_cache
assert_linux_release_matrix_required

if ! "$repo_root/scripts/target_toolchain_available.sh" x86_64-linux-gnu >/dev/null 2>&1; then
  echo "host x86_64-linux-gnu toolchain availability check failed" >&2
  exit 1
fi

assert_host_debug_target Linux x86_64 x86_64-linux-gnu x86_64
assert_host_debug_target Linux aarch64 aarch64-linux-gnu aarch64
assert_host_debug_target Linux armv7l armhf-linux-gnu arm
linux_deps_output=$(
  VECTIS_DEPS_DRY_RUN=1 "$repo_root/scripts/deps.sh" deps-x86_64-linux-gnu
)
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^libpid0_enabled=1$'; then
  echo "Linux dependency preset did not enable libpid0" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^libpid0_version=0\.4\.2$'; then
  echo "Linux dependency preset did not pin libpid0 0.4.2" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
for expected in \
  '^system_sha256=0bbb1cbaf60b0a94fb5a6b3756123088b45e2bef9e38079038f22e3c07febb2e$' \
  '^liblockdc_sha256=fcce40120a8e6c6990efdb4d54c427011619abb280a8ecbd4644b720eda7cbb8$' \
  '^lonejson_sha256=e04f80b907d92f7e38f825fbd339297e85372fc1ce110abb9a93715ee450ece3$' \
  '^pslog_sha256=7981ce7e60f6f1e144042e7a9192bb661472756ae34336fb0c2ed8316b31945f$' \
  '^cai_sha256=46d7c0c88633b932b98d6e6e0c035d6d4c4c24e8576122961d6e5a8affa78b5d$' \
  '^lql_sha256=a32b3ecc33b0634df23c630843b1c2c16a8a2caa947109a33bad20965e47a399$' \
  '^lql_lua_sha256=b440ce543586ebfc9aafd0e09a700126b9d62d85b8c34ae2ac19b0990db28438$' \
  '^softline_sha256=5d5e662269cf5bae9276f1ba7216dfb7e63127ea89c7c9b5f23cb33dbc970012$'
do
  if ! printf '%s\n' "$linux_deps_output" | grep -Eq "$expected"; then
    echo "Linux dependency preset did not expose expected upgraded dependency pin: $expected" >&2
    printf '%s\n' "$linux_deps_output" >&2
    exit 1
  fi
done
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^pslog_sha256='; then
  echo "Linux dependency preset did not expose libpslog metadata" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$linux_deps_output" | grep -Eq '^softline_sha256='; then
  echo "Linux dependency preset did not expose softline metadata" >&2
  printf '%s\n' "$linux_deps_output" >&2
  exit 1
fi
darwin_deps_output=$(
  VECTIS_DEPS_DRY_RUN=1 "$repo_root/scripts/deps.sh" deps-arm64-apple-darwin
)
if ! printf '%s\n' "$darwin_deps_output" | grep -Eq '^libpid0_enabled=0$'; then
  echo "Darwin dependency preset did not disable libpid0" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
if printf '%s\n' "$darwin_deps_output" | grep -Eq '^libpid0_version='; then
  echo "Darwin dependency preset exposed libpid0 version metadata" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
if ! printf '%s\n' "$darwin_deps_output" | grep -Eq '^softline_sha256='; then
  echo "Darwin dependency preset did not expose softline metadata" >&2
  printf '%s\n' "$darwin_deps_output" >&2
  exit 1
fi
if VECTIS_DEPS_DRY_RUN=1 \
  VECTIS_HOST_UNAME_S=Darwin \
  VECTIS_HOST_UNAME_M=arm64 \
  "$repo_root/scripts/deps.sh" deps-host-debug >/dev/null 2>&1; then
  echo "deps-host-debug accepted unsupported Darwin host" >&2
  exit 1
fi

override_version=$(VECTIS_VERSION_OVERRIDE=1.2.3 "$repo_root/scripts/release_version.sh")
if [ "$override_version" != "1.2.3" ]; then
  echo "VECTIS_VERSION_OVERRIDE did not drive release version" >&2
  exit 1
fi

printf '%s\n' '98.76.54' >"$version_path"
git_version=$("$repo_root/scripts/release_version.sh")
if [ "$git_version" = "98.76.54" ]; then
  echo "git worktree version detection read ignored /VERSION" >&2
  exit 1
fi

rm -rf "$version_work"
mkdir -p "$version_work/scripts"
cp "$repo_root/scripts/release_version.sh" "$version_work/scripts/release_version.sh"
git -C "$version_work" init -q
git -C "$version_work" config user.email lifecycle@example.invalid
git -C "$version_work" config user.name "Lifecycle Test"
git -C "$version_work" add scripts/release_version.sh
git -C "$version_work" commit -q -m 'test: seed version worktree'
git -C "$version_work" tag v1.2.3
tagged_override=$(VECTIS_VERSION_OVERRIDE=9.9.9 "$version_work/scripts/release_version.sh")
if [ "$tagged_override" != "1.2.3" ]; then
  echo "exact lightweight HEAD tag did not take precedence over override" >&2
  exit 1
fi
git -C "$version_work" tag -d v1.2.3 >/dev/null
untagged_override=$(VECTIS_VERSION_OVERRIDE=9.9.9 "$version_work/scripts/release_version.sh")
if [ "$untagged_override" != "9.9.9" ]; then
  echo "untagged git worktree did not accept explicit override" >&2
  exit 1
fi
printf '%s\n' '7.7.7' >"$version_work/VERSION"
untagged_version=$("$version_work/scripts/release_version.sh")
if [ "$untagged_version" != "0.0.0" ]; then
  echo "untagged git worktree did not resolve to 0.0.0" >&2
  exit 1
fi
git -C "$version_work" tag -a v2.0.0 -m 'annotated test tag'
annotated_version=$("$version_work/scripts/release_version.sh")
if [ "$annotated_version" != "0.0.0" ]; then
  echo "annotated HEAD tag was accepted as release version" >&2
  exit 1
fi

printf '%s\n' 'must not ship' >"$untracked_source_probe"
mkdir -p "$source_stage_dist"
source_archive=$(VECTIS_DIST_DIR="$source_stage_dist" "$repo_root/scripts/stage_release_sources.sh")
if tar -tzf "$source_archive" | grep -Eq '/vectis-untracked-source-probe\.txt$'; then
  echo "source archive included a non-ignored untracked worktree file" >&2
  exit 1
fi
if tar -xOzf "$source_archive" "vectis-$("$repo_root/scripts/release_version.sh")/RELEASE_MANIFEST" |
   grep -Eq '^vectis-untracked-source-probe\.txt$'; then
  echo "source archive manifest included a non-ignored untracked worktree file" >&2
  exit 1
fi

for preset in \
  x86_64-linux-gnu \
  x86_64-linux-musl \
  aarch64-linux-gnu \
  aarch64-linux-musl \
  armhf-linux-gnu \
  armhf-linux-musl \
  arm64-apple-darwin
do
  assert_contains "$repo_root/CMakePresets.json" "\"VECTIS_TARGET_ID\":[[:space:]]*\"$preset\""
done

for target in \
  print-release-version \
  format-check \
  asan \
  fuzz-smoke \
  package-source \
  package-source-smoke \
  package-checksums \
  package-verify \
  verify-release-archives \
  verify-release-privacy \
  verify-release-matrix \
  release-darwin-smoke-bundle \
  release-matrix \
  prerelease-live \
  prerelease-hardening \
  release \
  finalize-slice \
  prerelease \
  test-service-runtime-lifecycle \
  test-lua-facade-matrix \
  test-darwin-pack-signature \
  lua-test \
  lua-env \
  clean-dist \
  valgrind \
  lifecycle-version-contract \
  test-cpkt-toolchains
do
  assert_contains "$repo_root/Makefile" "^$target:"
done
assert_contains "$repo_root/Makefile" 'scripts/test_service_runtime_lifecycle_audit\.sh'
assert_contains "$repo_root/scripts/test_service_runtime_lifecycle_audit.sh" \
  'service runtime lifecycle audit ok'
assert_contains "$repo_root/Makefile" 'VECTIS_LIVE_OAUTH2_ENABLE=1'
assert_contains "$repo_root/Makefile" 'VECTIS_OPCUA_PUBSUB_LIVE=1'
assert_contains "$repo_root/scripts/test-live-oauth2.sh" 'VECTIS_LIVE_OAUTH2_ENABLE'
assert_contains "$repo_root/scripts/test-live-oauth2.sh" 'SKIP: set VECTIS_LIVE_OAUTH2_ENABLE=1'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'server:static_embedded'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'server:webdav_embedded_site'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'vectis_auth_provider'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'OAuth2/OIDC'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'startconsumer'
assert_contains "$repo_root/docs/pack-embedded-filesystem-auth-audit.md" 'does not copy Landed site assets'
assert_contains "$repo_root/TODO.md" '\[x\] Add generated or checked API docs for C and Lua'
assert_contains "$repo_root/docs/api.md" 'include/vectis/vectis\.h'
assert_contains "$repo_root/docs/api.md" '\[Lua surface\]\(lua\.md\)'
assert_contains "$repo_root/docs/api.md" '\[Lua coverage matrix\]\(lua-coverage-matrix\.md\)'
assert_contains "$repo_root/docs/api.md" 'vectis_status_string\(\)'
assert_contains "$repo_root/docs/api.md" 'vectis_register_route\(\)'
assert_contains "$repo_root/docs/api.md" 'vectis_response_\*'
assert_contains "$repo_root/docs/api.md" 'vectis_webdav_mount_config'
assert_contains "$repo_root/docs/api.md" 'vectis_auth_routes_config'
assert_contains "$repo_root/docs/api.md" 'vectis_auth_basic_authorization\(\)'
assert_contains "$repo_root/docs/api.md" 'vectis_openapi_document'
assert_contains "$repo_root/docs/api.md" 'vectis_lockd_config'
assert_contains "$repo_root/docs/api.md" 'vectis_http_client_config'
assert_contains "$repo_root/docs/api.md" 'vectis_ssh_config'
assert_contains "$repo_root/docs/api.md" 'vectis_mqtt_config'
assert_contains "$repo_root/docs/api.md" 'Certificates'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_cert_info'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_cert_inspect_bundle'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_cert_info_cleanup'
assert_contains "$repo_root/src/vectis.c" 'vectis_cert_inspect_bundle'
assert_contains "$repo_root/tests/unit/test_vectis_certs.c" 'subject_alt_names'
assert_contains "$repo_root/tests/unit/test_vectis_certs.c" 'serial_hex'
assert_contains "$repo_root/docs/lua-certs.md" 'vectis_cert_inspect_bundle'
assert_contains "$repo_root/TODO.md" '\[x\] Add source-backed C SDK certificate bundle inspection'
assert_contains "$repo_root/docs/api.md" '\[Lua auth\]\(lua-auth\.md\)'
assert_contains "$repo_root/docs/api.md" '\[Lua SSH\]\(lua-ssh\.md\)'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_status_string'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_register_route'
assert_contains "$repo_root/include/vectis/vectis.h" 'vectis_http_client_config'

assert_contains "$repo_root/cmake/toolchains/x86_64-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(x86_64-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/x86_64-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(x86_64-linux-musl\)'
assert_contains "$repo_root/cmake/toolchains/aarch64-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(aarch64-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/aarch64-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(aarch64-linux-musl\)'
assert_contains "$repo_root/cmake/toolchains/armhf-linux-gnu.cmake" 'cpkt_configure_bootlin_toolchain\(armhf-linux-gnu\)'
assert_contains "$repo_root/cmake/toolchains/armhf-linux-musl.cmake" 'cpkt_configure_bootlin_toolchain\(armhf-linux-musl\)'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktOpcUa CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktSus CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(CpktAudio CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'find_package\(liblql CONFIG REQUIRED'
assert_contains "$repo_root/CMakeLists.txt" 'OBJECT_DEPENDS "\$\{vectis_dependency_manifest\}"'
assert_contains "$repo_root/cmake/vectis.pc.in" 'cpkt-opcua cpkt-sus cpkt-audio'
assert_contains "$repo_root/cmake/vectis.pc.in" 'liblql'

echo "lifecycle contracts ok"
