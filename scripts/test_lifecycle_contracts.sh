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
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'S_SRC\+=src/ljson\.c'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'CFLAGS\+=-DKORE_USE_LONEJSON'
  assert_contains "$repo_root/scripts/verify-kore-patches.sh" 'LDFLAGS\+=-L\$\(LONEJSON_PATH\)/lib -llonejson'
}

assert_kore_static_runtime_contract() {
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
  assert_contains "$repo_root/docs/lua-lockd.md" 'vectis\.lockd\.raw'
  assert_contains "$repo_root/docs/lua-lockd.md" 'client_bundle = "embedded"'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'client_bundle == "embedded"'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'embedded_lockd_bundle_source'
  assert_contains "$repo_root/lua/vectis/lockd.lua" 'function M.with_client'
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
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "opcua"'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'luaopen_opcua'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_connect'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_read'
  assert_contains "$repo_root/src/vectis_opcua_lua.c" 'cpkt_opcua_client_write'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_opcua_e2e'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local;integration"'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'cpkt_opcua_server_startup'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'cpkt_lua_runtime_register_c_module'
  assert_contains "$repo_root/tests/opcua_lua_e2e.c" 'client:write'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("opcua"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.node_id_numeric'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.value_string'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'opcua\.client'
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
  assert_contains "$repo_root/examples/lua/audio_devices.lua" 'VECTIS_LUA_AUDIO_DEVICE_EXAMPLE'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_audio_devices'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'examples/lua/audio_sus\.lua'
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
  assert_contains "$matrix" '\| dep:opcua \|'
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
  assert_contains "$matrix" '\| workflow:http-client \|'
  assert_contains "$matrix" '\| workflow:sftp-curl \|'
  assert_contains "$matrix" '\| workflow:ssh-exec \|'
  assert_contains "$matrix" '\| workflow:scp \|'
  assert_contains "$matrix" '\| workflow:sftp-libssh2 \|'
  assert_contains "$matrix" '\| workflow:xml \|'
  assert_contains "$matrix" '\| workflow:dsv \|'
  assert_contains "$matrix" '\| workflow:lockd-state \|'
  assert_contains "$matrix" '\| workflow:lockd-queue \|'
  assert_contains "$matrix" '\| workflow:server-consumer \|'
  assert_contains "$matrix" '\| workflow:opcua-client \|'
  assert_contains "$matrix" '\| workflow:opcua-server \|'
  assert_contains "$matrix" '\| workflow:opcua-async \|'
  assert_contains "$matrix" '\| workflow:cai \|'
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
  assert_contains "$repo_root/TODO.md" '\[x\] Add Lua OpenAPI route metadata'
  assert_contains "$repo_root/TODO.md" '\[x\] Add explicit file-backed Lua `spooled_source` route responses'
  assert_contains "$matrix" 'file-backed `spooled_source` responses'
  assert_contains "$matrix" 'true live response streaming and SSE remain separate missing surfaces'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_response_spooled_source'
  assert_contains "$repo_root/docs/lua-server.md" '`spooled_source` is a file-backed response source'
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
  assert_contains "$repo_root/TODO.md" '\[ \] Add Vectis-owned Lua modules for Kore and broader libssh2 coverage'
  assert_contains "$repo_root/TODO.md" '\[x\] Add one-shot Lua libssh2-backed SFTP filesystem helpers'
  assert_contains "$repo_root/TODO.md" '\[x\] Add broader Lua libssh2-backed SFTP session, file open/read/write/stat, and directory iteration handles'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a Lua example for stateful SFTP/libssh2 lower-level handle operations'
  assert_contains "$repo_root/docs/lua-certs.md" 'require\("openssl"\)'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_stat'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_chmod'
  assert_contains "$repo_root/docs/lua-ssh.md" 'sftp_open'
  assert_contains "$repo_root/docs/lua-ssh.md" 'session:open_file'
  assert_contains "$repo_root/docs/lua-ssh.md" 'dir:read'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_stat'
  assert_contains "$repo_root/src/vectis_cli.c" 'sftp_chmod'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_open'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_session_open_file'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_ssh_sftp_dir_read'
  assert_contains "$repo_root/tests/lua/ssh_sftp.cmake" 'sftp_stat'
  assert_contains "$repo_root/tests/lua/ssh_sftp.cmake" 'sftp_open'
  assert_contains "$repo_root/examples/lua/sftp_handles.lua" 'sftp_open'
  assert_contains "$repo_root/examples/lua/sftp_handles.lua" 'open_file'
  assert_contains "$repo_root/scripts/test-e2e.sh" 'lua stateful sftp handles'
  assert_contains "$matrix" 'sftp_stat'
  assert_contains "$matrix" 'stateful SFTP session/file/directory receivers exist'
  assert_contains "$matrix" 'opt-in SSH/SFTP e2e'
  assert_contains "$matrix" 'raw SSH channels and advanced host-key workflows remain missing'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_lua_facade_contracts'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'vectis-lua-facade-contracts-ok'
  assert_contains "$repo_root/TODO.md" '\[x\] Extend Vectis-owned Lua `nil, err` objects with C error source metadata'
  assert_contains "$repo_root/docs/lua-conventions.md" 'err\.source'
  assert_contains "$repo_root/docs/lua-conventions.md" 'err\.dependency_code'
  assert_contains "$repo_root/src/vectis_cli.c" '"ERROR_SOURCE_VECTIS"'
  assert_contains "$repo_root/src/vectis_cli.c" 'error_source_string'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'error_source_string'
  assert_contains "$repo_root/tests/lua/facade_contracts.cmake" 'source_code'
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
  assert_contains "$lua_index" 'Raw Dependency Modules'
  assert_contains "$lua_index" 'Vectis Workflow Modules'
  assert_contains "$lua_index" '\[Lua facade conventions\]\(lua-conventions\.md\)'
  assert_contains "$lua_index" '\[Lua coverage matrix\]\(lua-coverage-matrix\.md\)'
  assert_contains "$lua_index" '\[SUS and audio contract\]\(lua-sus-audio-contract\.md\)'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Expected runtime failures return structured values'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Programmer misuse raises a Lua error'
  assert_contains "$repo_root/docs/lua-conventions.md" 'Do not describe a materialized or spooled path as streaming'
  assert_contains "$lua_index" '`lockdc`'
  assert_contains "$lua_index" '`lonejson`'
  assert_contains "$lua_index" '\[Lua pslog\]\(lua-pslog\.md\)'
  assert_contains "$lua_index" '\[Lua lql\]\(lua-lql\.md\)'
  assert_contains "$lua_index" '`cai`'
  assert_contains "$lua_index" '`libmdf`'
  assert_contains "$lua_index" '`softline`'
  assert_contains "$lua_index" '`curl`'
  assert_contains "$lua_index" '`openssl`'
  assert_contains "$lua_index" '`zlib`'
  assert_contains "$lua_index" '`opcua`'
  assert_contains "$lua_index" '`audio`'
  assert_contains "$lua_index" '`sus`'
  assert_contains "$lua_index" '`vectis\.auth`'
  assert_contains "$lua_index" '`vectis\.cert`'
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
  assert_contains "$lua_index" 'direct preloaded modules'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.auth"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.cert"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.embedded"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.server"'
  assert_contains "$repo_root/src/vectis_cli.c" 'runtime, "vectis\.ssh"'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "auth", "vectis\.auth"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "server", "vectis\.server"\)'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_set_required_module\(lua, "embedded", "vectis\.embedded"\)'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"vectis\.auth"'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" 'loaded\["vectis"\]\.auth == loaded\["vectis\.auth"\]'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("vectis\.auth"\)'
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
  assert_contains "$repo_root/include/vectis/webdav.h" 'const char \*root_dir'
  assert_contains "$repo_root/src/vectis_cli.c" '"root_dir"'
  assert_contains "$repo_root/src/vectis_webdav.c" 'vectis_webdav_direct_root'
  assert_contains "$repo_root/docs/lua-webdav.md" 'root_dir` to serve a direct'
  assert_contains "$repo_root/tests/lua/webdav_server.cmake" 'root_dir = root_dir'
  assert_contains "$matrix" 'direct mutable disk `root_dir` mounts'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.markdown'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.markdown_stream'
  assert_contains "$repo_root/lua/vectis/terminal.lua" 'function M\.editor'
  assert_contains "$repo_root/docs/lua-terminal.md" 'terminal\.markdown_stream'
  assert_contains "$repo_root/examples/lua/terminal_tools.lua" 'terminal\.markdown'
  assert_contains "$repo_root/examples/lua/terminal_tools.lua" 'terminal\.editor'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_terminal_tools'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'terminal_tools\.lua'
  assert_contains "$matrix" '`vectis\.terminal` covers Markdown render'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.new'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.log_error'
  assert_contains "$repo_root/lua/vectis/log.lua" 'function M\.from_env'
  assert_contains "$repo_root/docs/lua-log.md" 'log\.log_error'
  assert_contains "$repo_root/docs/lua-log.md" 'log\.from_env'
  assert_contains "$repo_root/examples/lua/logging.lua" 'log\.log_error'
  assert_contains "$repo_root/examples/lua/logging.lua" 'log\.from_env'
  assert_contains "$repo_root/docs/lua-pslog.md" 'pslog\.new_json'
  assert_contains "$repo_root/docs/lua-pslog.md" 'pslog\.from_env'
  assert_contains "$repo_root/docs/lua-lql.md" 'client:selector_parse'
  assert_contains "$repo_root/docs/lua-lql.md" 'client:apply_string_spooled'
  assert_contains "$repo_root/tests/CMakeLists.txt" 'vectis_example_lua_logging'
  assert_contains "$repo_root/tests/lua/example_local_facades_pack.cmake" 'logging\.lua'
  assert_contains "$matrix" '`vectis\.log` adds JSON logger defaults'
  assert_contains "$matrix" 'workflow:logging'
  assert_contains "$repo_root/include/vectis/auth.h" 'vectis_auth_basic_authorization'
  assert_contains "$repo_root/src/vectis_auth.c" 'EVP_EncodeBlock'
  assert_contains "$repo_root/src/vectis_cli.c" 'basic_authorization'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.auth\.basic_authorization'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'vectis\.auth\.basic_authorization'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'require\("vectis\.rest"\)'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:auth_json'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'server:json'
  assert_contains "$repo_root/examples/lua/api_server.lua" 'rest\.client'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'require\("vectis\.rest"\)'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'server:json'
  assert_contains "$repo_root/examples/lua/downstream_api.lua" 'rest\.client'
  assert_contains "$repo_root/docs/lua-auth.md" 'basic_authorization'
  assert_contains "$matrix" 'Basic Authorization formatting'
  assert_contains "$repo_root/TODO.md" 'raw zlib Lua facade'
  assert_contains "$repo_root/src/vectis_cli.c" 'luaopen_zlib'
  assert_contains "$repo_root/src/vectis_cli.c" 'cpkt_lua_runtime_register_c_module\(runtime, "zlib"'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'require\("zlib"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'zlib\.gzip'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'zlib\.gzip_file'
  assert_contains "$repo_root/TODO.md" '\[x\] Add a top-level `vectis\.libs` namespace'
  assert_contains "$repo_root/src/vectis_cli.c" 'vectis_lua_push_libs_table'
  assert_contains "$repo_root/src/vectis_cli.c" 'lua_setfield\(lua, -2, "libs"\)'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.libs\.lockdc == lockdc'
  assert_contains "$repo_root/tests/lua/smoke.lua" 'vectis\.libs\.zlib == zlib'
  assert_contains "$lua_index" '`require\("vectis"\)\.libs`'
  assert_contains "$matrix" '`vectis\.libs` collects the bundled dependency facades'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'require\("zlib"\)'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'zlib\.decompress'
  assert_contains "$repo_root/examples/lua/local_data_pipeline.lua" 'zlib\.decompress_file'
  assert_contains "$repo_root/docs/lua-zlib.md" 'file-backed'
  assert_contains "$repo_root/docs/api.md" '\[Lua zlib\]\(lua-zlib\.md\)'
  assert_contains "$repo_root/examples/README.md" 'zlib string/file compression'
  assert_contains "$matrix" 'file-backed bounded transforms'
  assert_contains "$matrix" 'packed local data pipeline example coverage'
  assert_contains "$repo_root/scripts/verify_vectis_lua_preloads.sh" '"zlib"'
}

assert_luarocks_artifact_rejected() {
  dist=$luarocks_dist
  version=0.0.0
  root_name=vectis-$version-x86_64-linux-musl
  artifact=vectis-$version-x86_64-linux-musl.tar.gz
  root="$dist/$root_name"

  rm -rf "$dist"
  mkdir -p \
    "$root/include/vectis" \
    "$root/lib/cmake/vectis" \
    "$root/lib/pkgconfig" \
    "$root/share/doc/vectis" \
    "$root/.luarocks"
  printf '#define VECTIS_VERSION "%s"\n' "$version" >"$root/include/vectis/vectis_version.h"
  printf '%s\n' '# test config' >"$root/lib/cmake/vectis/vectisConfig.cmake"
  printf '%s\n' '# test config version' >"$root/lib/cmake/vectis/vectisConfigVersion.cmake"
  printf '%s\n' 'Name: vectis' >"$root/lib/pkgconfig/vectis.pc"
  printf '%s\n' 'license' >"$root/share/doc/vectis/LICENSE"
  printf '%s\n' 'readme' >"$root/share/doc/vectis/README.md"
  printf '%s\n' 'must not ship' >"$root/.luarocks/bad.rock"
  tar -C "$dist" -czf "$dist/$artifact" "$root_name"
  (cd "$dist" && sha256sum "$artifact" >"vectis-$version-CHECKSUMS")

  if VECTIS_VERSION=$version VECTIS_DIST_DIR=$dist \
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

if [ -f "$version_path" ]; then
  had_version=1
  saved_version=$(cat "$version_path")
fi

assert_contains "$repo_root/.gitignore" '^/VERSION$'
assert_contains "$repo_root/scripts/package.sh" 'target_toolchain_available\.sh'
assert_contains "$repo_root/CMakeLists.txt" 'generated/pkgconfig/vectis\.pc'
assert_contains "$repo_root/CMakeLists.txt" 'pkgconfig"\)'
assert_contains "$repo_root/cmake/vectis.pc.in" '^Name: vectis$'
assert_contains "$repo_root/cmake/package_archive.cmake" 'share/c\.pkt\.systems'
assert_contains "$repo_root/cmake/package_archive.cmake" 'CMAKE_INSTALL_NAME_TOOL'
assert_contains "$repo_root/cmake/package_archive.cmake" '@rpath/\$\{vectis_darwin_dep_name\}'
assert_contains "$repo_root/cmake/package_darwin_smoke_bundle.cmake" '@executable_path/\.\./lib'
assert_contains "$repo_root/scripts/verify_installed_sdk.sh" 'pkg-config --static --cflags --libs vectis'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK missing pkg-config metadata'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains dependency source tree'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'binary SDK contains LuaRocks artifacts'
assert_contains "$repo_root/scripts/verify_release_artifacts.sh" 'verify_vectis_lua_preloads\.sh'
assert_contains "$repo_root/scripts/package.sh" 'verify_vectis_lua_preloads\.sh'
assert_contains "$repo_root/TODO.md" '\[x\] Statically preload every bundled raw Lua dependency facade'
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
assert_contains "$repo_root/tests/CMakeLists.txt" 'LABELS "lua;smoke;local"'
assert_contains "$repo_root/CMakeLists.txt" 'target_compile_options\(\$\{target\} PRIVATE'
assert_contains "$repo_root/CMakeLists.txt" 'Werror'
assert_contains "$repo_root/CMakeLists.txt" '_DARWIN_C_SOURCE'
assert_contains "$repo_root/CMakeLists.txt" 'libpid0_enabled "0"'
assert_contains "$repo_root/scripts/deps.sh" 'libpid0_enabled=\$pid0_enabled'
assert_contains "$repo_root/examples/CMakeLists.txt" 'target_compile_options\(\$\{target_name\} PRIVATE'
assert_contains "$repo_root/examples/CMakeLists.txt" 'Werror'
assert_no_landed_test_assets
assert_action_surface_contract
assert_lua_example_dx_contract
assert_kore_lonejson_contract
assert_kore_static_runtime_contract
assert_lockdc_lua_runtime_contract
assert_lql_lua_runtime_contract
assert_opcua_lua_runtime_contract
assert_audio_sus_lua_runtime_contract
assert_lua_coverage_matrix_contract
assert_luarocks_artifact_rejected

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
  '^cai_sha256=e344102fa5b46e8c05d67a5120ea0c74bf9ee8ad9ec0bc01e08ea5ccc1f1bdc9$' \
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
  asan \
  fuzz-smoke \
  package-source \
  package-source-smoke \
  package-checksums \
  package-verify \
  verify-release-archives \
  verify-release-privacy \
  release-darwin-smoke-bundle \
  release-matrix \
  prerelease-live \
  prerelease-hardening \
  release \
  finalize-slice \
  prerelease \
  lua-test \
  lua-env \
  clean-dist \
  valgrind \
  lifecycle-version-contract \
  test-cpkt-toolchains
do
  assert_contains "$repo_root/Makefile" "^$target:"
done
assert_contains "$repo_root/Makefile" 'VECTIS_LIVE_OAUTH2_ENABLE=1'
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
