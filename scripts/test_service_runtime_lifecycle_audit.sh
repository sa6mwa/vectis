#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

require_egrep() {
  file=$1
  pattern=$2
  label=$3
  if ! grep -Eq "$pattern" "$file"; then
    printf 'service runtime lifecycle audit failed: %s\n' "$label" >&2
    printf 'missing pattern in %s: %s\n' "$file" "$pattern" >&2
    exit 1
  fi
}

reject_egrep() {
  file=$1
  pattern=$2
  label=$3
  if grep -Eq "$pattern" "$file"; then
    printf 'service runtime lifecycle audit failed: %s\n' "$label" >&2
    printf 'forbidden pattern in %s: %s\n' "$file" "$pattern" >&2
    exit 1
  fi
}

require_runtime_case() {
  name=$1
  require_egrep "$repo_root/tests/unit/test_vectis_runtime.c" \
    "strcmp\\(name, \"$name\"\\)" \
    "runtime unit selector is registered: $name"
}

require_ctest() {
  name=$1
  if ! grep -Eq "add_test\\(NAME $name" "$repo_root/tests/CMakeLists.txt" &&
     ! grep -Eq "add_test\\(NAME $name" "$repo_root/examples/CMakeLists.txt"; then
    printf 'service runtime lifecycle audit failed: CTest is registered: %s\n' \
      "$name" >&2
    exit 1
  fi
}

require_todo_done() {
  pattern=$1
  require_egrep "$repo_root/TODO.md" "\\[x\\] $pattern" \
    "TODO marks completed runtime surface: $pattern"
}

assert_core_runtime_spec_evidence() {
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'Status: draft implementation authority' \
    "service runtime spec remains authoritative"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'Kore must never fork workers from a process' \
    "Kore fork boundary invariant is documented"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'T1: Direct Kore Runtime' \
    "T1 topology is specified"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'T2: Supervised Kore Runtime' \
    "T2 topology is specified"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'T3: Service-Only Runtime' \
    "T3 topology is specified"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'Lua callbacks run only in the Lua state and process' \
    "Lua owner-state callback invariant is documented"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'Ordinary in-process pointers and `vectis_mailbox` handles are not cross-process' \
    "mailbox process-local invariant is documented"
  require_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'The shutdown grace is one app-level deadline' \
    "shared shutdown deadline invariant is documented"
}

assert_runtime_unit_evidence() {
  for case_name in \
    server_config_validation \
    runtime_control_frame_contract \
    route_body_policy_validation \
    get_only_server_does_not_create_spool_dir \
    upload_server_rejects_file_spool_path \
    upload_server_rejects_unsafe_spool_dir \
    metrics_surface \
    supervised_metrics_persistence_worker \
    metrics_persistence_stop_honors_shutdown_grace \
    direct_supervision_policy_rejects_app_services \
    consumer_service_declaration_before_routes \
    managed_service_declaration_before_routes \
    managed_service_explicit_start_before_routes_defers \
    routes_reject_materialized_managed_services \
    kore_start_rejects_extra_thread \
    kore_start_reports_occupied_listener \
    supervised_wait_reports_consumer_service_exit \
    supervised_wait_reports_consumer_service_clean_exit \
    active_consumer_start_preserves_restart_request \
    supervised_child_exit_stops_consumer_service \
    supervised_repeated_start_stop \
    route_backed_start_without_services_is_supervised \
    runtime_phase_order_contract \
    runtime_start_failure_rolls_back_services \
    supervised_managed_service_lifecycle \
    supervised_routes_wait_for_full_app_readiness \
    managed_service_stop_honors_shutdown_grace \
    managed_services_share_shutdown_grace \
    app_close_joins_timed_out_managed_service \
    managed_service_detached_after_app_close \
    consumer_service_detached_after_app_close \
    managed_service_inherits_app_logger \
    managed_service_logger_disabled \
    managed_service_direct_stop_clears_started \
    managed_service_stop_cancels_pending_start \
    managed_service_run_materializes_service_only \
    kore_start_waits_for_transient_thread_teardown \
    service_only_curl_worker_mailbox_http \
    curl_worker_stop_wakes_idle_mailbox_wait \
    supervised_opcua_server_service_lifecycle \
    supervised_shutdown_deadline_kills_stopped_runtime \
    service_only_wait_reports_consumer_service_exit \
    consumer_service_run_until_materializes_descriptor \
    service_failure_continue_waits_for_signal \
    managed_service_restart_replaces_done_monitor \
    supervised_wait_reports_managed_service_exit
  do
    require_runtime_case "$case_name"
  done
  require_egrep "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_supervised_routes_wait_for_full_app_readiness' \
    "route-ready 503 behavior has unit coverage"
  require_egrep "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_managed_services_share_shutdown_grace' \
    "managed services share one shutdown deadline"
  require_egrep "$repo_root/tests/unit/test_vectis_runtime.c" \
    'assert_supervised_child_exit_stops_consumer_service' \
    "child death stops supervisor services"
}

assert_service_family_surface_evidence() {
  for api_name in \
    managed_service \
    opcua_server_service \
    curl_worker_service \
    cai_worker_service \
    audio_worker_service \
    sus_worker_service
  do
    require_egrep "$repo_root/include/vectis/vectis.h" \
      "vectis_status \\(\\*$api_name\\)" \
      "public app receiver exposes $api_name"
    require_egrep "$repo_root/include/vectis/vectis.h" \
      "vectis_${api_name}_new" \
      "public constructor wrapper exposes $api_name"
  done
  require_egrep "$repo_root/include/vectis/vectis.h" \
    'vectis_consumer_service_state_get' \
    "lockdc consumer service exposes copied state"
  require_egrep "$repo_root/include/vectis/vectis.h" \
    'struct vectis_managed_service' \
    "managed service receiver shell is public"
  require_egrep "$repo_root/src/vectis.c" \
    'vectis_app_start_requested_managed_services' \
    "managed services start through app lifecycle"
  require_egrep "$repo_root/src/vectis.c" \
    'vectis_app_stop_managed_services' \
    "managed services stop through app lifecycle"
  require_egrep "$repo_root/src/vectis.c" \
    'vectis_app_start_requested_consumer_services' \
    "lockdc consumers start through app lifecycle"
  require_egrep "$repo_root/src/vectis.c" \
    'vectis_app_stop_consumer_services' \
    "lockdc consumers stop through app lifecycle"
}

assert_lua_runtime_evidence() {
  for module_name in \
    vectis.audio_worker \
    vectis.cai_worker \
    vectis.curl_worker \
    vectis.mailbox \
    vectis.sus_worker
  do
    require_egrep "$repo_root/src/vectis_cli.c" "runtime, \"$module_name\"" \
      "Lua preload registers $module_name"
  done
  for method_name in \
    consumer_service \
    consumer_service_states \
    opcua_server_service \
    opcua_server_service_states \
    curl_worker_service \
    curl_worker_service_states \
    cai_worker_service \
    cai_worker_service_states \
    audio_worker_service \
    audio_worker_service_states \
    sus_worker_service \
    sus_worker_service_states
  do
    require_egrep "$repo_root/src/vectis_cli.c" "\"$method_name\"" \
      "Lua server receiver exposes $method_name"
  done
  require_egrep "$repo_root/tests/lua/smoke.lua" \
    'direct Lua callbacks' \
    "Lua smoke rejects direct background callbacks"
  require_egrep "$repo_root/tests/lua/smoke.lua" \
    'pump_callback_failures' \
    "Lua mailbox pump callback failures are observable"
  require_egrep "$repo_root/docs/lua-server.md" \
    'not shell commands or external sleep loops' \
    "Lua server docs require Vectis waits"
}

assert_integration_and_example_evidence() {
  require_ctest vectis_server_signal_shutdown
  require_ctest vectis_example_lua_metrics_persistent
  require_ctest vectis_example_lua_metrics_authenticated
  require_ctest vectis_example_lua_curl_worker_service
  require_ctest vectis_example_lua_cai_worker_service
  require_ctest vectis_example_lua_audio_worker_service
  require_ctest vectis_example_lua_sus_worker_service
  require_ctest vectis_example_lua_cai_worker_service_supervised
  require_ctest vectis_example_lua_webdav_fileserver
  require_ctest vectis_example_lua_api_server_pack
  require_ctest vectis_example_mailbox_request_reply
  require_egrep "$repo_root/examples/lua/consumer_service.lua" \
    'server:consumer_service' \
    "Lua consumer service example declares service"
  require_egrep "$repo_root/examples/kore/kore_webdav_lockd_consumer_e2e.c" \
    'consumer_service' \
    "C WebDAV plus lockdc consumer scenario exists"
  require_egrep "$repo_root/examples/concurrency/mailbox_request_reply.c" \
    'run_opcua_monitor_adapter' \
    "C mailbox example covers OPC UA monitor handoff"
  require_egrep "$repo_root/tests/lua/server_signal_shutdown.sh" \
    'run_lua_case TERM' \
    "Lua server signal shutdown test sends SIGTERM"
  require_egrep "$repo_root/tests/lua/server_signal_shutdown.sh" \
    'run_c_case TERM' \
    "C server signal shutdown test sends SIGTERM"
}

assert_todo_and_no_stale_claims() {
  require_todo_done 'Complete support for one Vectis process to run a Kore-backed API/WebDAV server and an app-owned liblockdc `startconsumer` service simultaneously'
  require_todo_done 'Add a same-process scenario test that serves an API/WebDAV mount while receiving lockd messages through `startconsumer`'
  require_todo_done 'Implement the documented runtime phase/order contract'
  require_todo_done 'Add focused C runtime tests for the phase/order contract'
  require_todo_done 'Define Lua consumer-service runner behavior for the combined server-plus-consumer process model'
  require_todo_done 'Add the C-owned concurrency DX mailbox'
  require_todo_done 'Add the Lua mailbox facade with owner-state `pump\(\)` semantics'
  require_todo_done 'Add deterministic C and Lua scenario coverage for route-style mailbox request/reply'
  require_todo_done 'Add metrics and diagnostics hooks for mailbox depth'
  require_todo_done 'Add a generic Vectis metrics/stats handler'
  require_todo_done 'Add descriptor-backed `vectis_audio_worker_service`'
  require_todo_done 'Add descriptor-backed `vectis_sus_worker_service`'
  reject_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'Open Decisions Before Coding Beyond Lockdc' \
    "stale lockdc-only runtime planning section was removed"
  reject_egrep "$repo_root/docs/service-runtime-lifecycle-spec.md" \
    'TODO|not yet implemented|stub implementation' \
    "service runtime spec must not contain vague implementation placeholders"
}

assert_core_runtime_spec_evidence
assert_runtime_unit_evidence
assert_service_family_surface_evidence
assert_lua_runtime_evidence
assert_integration_and_example_evidence
assert_todo_and_no_stale_claims

printf '%s\n' 'service runtime lifecycle audit ok'
