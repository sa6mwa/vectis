#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

disk_endpoint=${VECTIS_E2E_LOCKD_DISK_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_DISK_PORT:-29441}}
s3_endpoint=${VECTIS_E2E_LOCKD_S3_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_S3_PORT:-29443}}
client_bundle=${VECTIS_E2E_LOCKD_CLIENT_BUNDLE:-$repo_root/devenv/volumes/lockd-config/client.pem}
ssh_port=${VECTIS_SSH_PORT:-29222}
mqtt_port=${VECTIS_MQTT_PORT:-21883}
default_kore_basic_port=$((28080 + ($$ % 1000) * 2))
kore_basic_port=${VECTIS_E2E_KORE_BASIC_PORT:-$default_kore_basic_port}
kore_lockd_port=${VECTIS_E2E_KORE_LOCKD_PORT:-$((kore_basic_port + 1))}
kore_workflow_port=${VECTIS_E2E_KORE_WORKFLOW_PORT:-$((kore_basic_port + 2))}
kore_downstream_port=${VECTIS_E2E_KORE_DOWNSTREAM_PORT:-$((kore_basic_port + 3))}
kore_static_port=${VECTIS_E2E_KORE_STATIC_PORT:-$((kore_basic_port + 4))}
work_dir=$(mktemp -d)
ssh_memory_key="$work_dir/vectis-e2e-ssh-key"
ssh_bad_host_key="$work_dir/vectis-e2e-bad-host-key"
ssh_known_hosts="$work_dir/vectis-e2e-known-hosts"
ssh_bad_known_hosts="$work_dir/vectis-e2e-bad-known-hosts"
server_pids=""

cleanup() {
  for pid in $server_pids; do
    kill -- "-$pid" >/dev/null 2>&1 || kill "$pid" >/dev/null 2>&1 || true
  done
  sleep 1
  for pid in $server_pids; do
    kill -9 -- "-$pid" >/dev/null 2>&1 || kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  rm -rf "$work_dir"
  if [ "${VECTIS_E2E_KEEP_DEVSERVICES:-0}" != "1" ]; then
    "$script_dir/dev-down.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

wait_for_http() {
  url=$1
  label=$2
  count=0
  while ! curl --max-time 3 -fsS "$url" >/dev/null 2>&1; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $url" >&2
      return 1
    fi
    sleep 1
  done
}

start_server() {
  label=$1
  logfile=$2
  shift 2

  if command -v setsid >/dev/null 2>&1; then
    setsid "$@" >"$logfile" 2>&1 &
  else
    "$@" >"$logfile" 2>&1 &
  fi
  pid=$!
  server_pids="$server_pids $pid"
  printf '[e2e] %s pid=%s\n' "$label" "$pid"
  sleep 1
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    printf '%s\n' "$label exited during startup" >&2
    sed 's/^/[server] /' "$logfile" >&2
    return 1
  fi
}

run_lockd_examples() {
  endpoint=$1
  label=$2

  printf '[e2e] lockd %s open\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_open"

  printf '[e2e] lockd %s lease save/load/release\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_lease"

  printf '[e2e] lockd %s enqueue/dequeue\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_enqueue"
  env LOCKD_ENDPOINT="$endpoint" LOCKD_CLIENT_BUNDLE="$client_bundle" \
    "$repo_root/build/debug/examples/vectis_example_lockd_dequeue"
}

run_lockd_failure_examples() {
  bad_bundle="$work_dir/bad-client.pem"

  printf '[e2e] lockd unavailable endpoint failure\n'
  if env LOCKD_ENDPOINT="https://127.0.0.1:1" LOCKD_CLIENT_BUNDLE="$client_bundle" \
      "$repo_root/build/debug/examples/vectis_example_lockd_lease"; then
    printf '%s\n' "lockd lease unexpectedly succeeded against unavailable endpoint" >&2
    return 1
  fi

  printf '%s\n' "not a pem bundle" >"$bad_bundle"
  printf '[e2e] lockd invalid client bundle failure\n'
  if env LOCKD_ENDPOINT="$disk_endpoint" LOCKD_CLIENT_BUNDLE="$bad_bundle" \
      "$repo_root/build/debug/examples/vectis_example_lockd_lease"; then
    printf '%s\n' "lockd lease unexpectedly succeeded with invalid client bundle" >&2
    return 1
  fi
}

run_lockd_lua_examples() {
  endpoint=$1
  label=$2

  printf '[e2e] lockd lua %s open/close\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    "$repo_root/build/debug/vectis" "$repo_root/tests/lua/lockdc_open_close.lua"

  printf '[e2e] lockd lua %s state\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    LOCKD_STATE_KEY="lua/e2e/$label/state-$$" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/lockd_state.lua"

  printf '[e2e] lockd lua %s queue\n' "$label"
  env LOCKD_ENDPOINT="$endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_NAMESPACE="examples" \
    LOCKD_QUEUE="lua-e2e-$label-$$" \
    "$repo_root/build/debug/vectis" "$repo_root/examples/lua/lockd_queue.lua"
}

run_service_examples() {
  printf '[e2e] mqtt publish\n'
  env VECTIS_MQTT_URL="mqtt://127.0.0.1:$mqtt_port" \
    "$repo_root/build/debug/examples/vectis_example_mqtt"

  printf '[e2e] ssh command\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] ssh command rejects wrong known_hosts pin\n'
  if env VECTIS_SSH_HOST="127.0.0.1" \
      VECTIS_SSH_PORT="$ssh_port" \
      VECTIS_SSH_USERNAME="vectis" \
      VECTIS_SSH_PASSWORD="vectispass" \
      VECTIS_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$repo_root/build/debug/examples/vectis_example_ssh"; then
    printf '%s\n' "libssh2 SSH unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] ssh command with memory private key\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PRIVATE_KEY_MEMORY_FILE="$ssh_memory_key" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] libssh2 sftp upload/download\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    VECTIS_SSH_KNOWN_HOSTS="$ssh_known_hosts" \
    "$repo_root/build/debug/examples/vectis_example_ssh_sftp"

  printf '[e2e] libssh2 sftp rejects wrong known_hosts pin\n'
  if env VECTIS_SSH_HOST="127.0.0.1" \
      VECTIS_SSH_PORT="$ssh_port" \
      VECTIS_SSH_USERNAME="vectis" \
      VECTIS_SSH_PASSWORD="vectispass" \
      VECTIS_SSH_KNOWN_HOSTS="$ssh_bad_known_hosts" \
      "$repo_root/build/debug/examples/vectis_example_ssh_sftp"; then
    printf '%s\n' "libssh2 SFTP unexpectedly accepted mismatched known_hosts pin" >&2
    return 1
  fi

  printf '[e2e] curl sftp upload/download\n'
  env VECTIS_SFTP_URL="sftp://127.0.0.1:$ssh_port" \
    VECTIS_SFTP_USERNAME="vectis" \
    VECTIS_SFTP_PASSWORD="vectispass" \
    "$repo_root/build/debug/examples/vectis_example_sftp"
}

run_downstream_http_examples() {
  printf '[e2e] downstream HTTP client/server\n'
  start_server "downstream http" "$work_dir/downstream-http.log" \
    env VECTIS_KORE_PORT="$kore_downstream_port" \
      "$repo_root/build/debug/examples/vectis_example_curl_downstream_e2e" server
  wait_for_http "http://127.0.0.1:$kore_downstream_port/health" "downstream HTTP server"
  env VECTIS_DOWNSTREAM_BASE_URL="http://127.0.0.1:$kore_downstream_port" \
    "$repo_root/build/debug/examples/vectis_example_curl_downstream_e2e" client
}

run_static_asset_examples() {
  status=
  body=

  printf '[e2e] kore static assets and traversal guard\n'
  start_server "static assets" "$work_dir/static-assets.log" \
    env VECTIS_KORE_PORT="$kore_static_port" \
      VECTIS_STATIC_ROOT="$work_dir/static-assets" \
      VECTIS_STATIC_INDEX="$work_dir/static-assets/index.html" \
      VECTIS_STATIC_APP="$work_dir/static-assets/app.js" \
      "$repo_root/build/debug/examples/vectis_example_kore_static_assets"
  wait_for_http "http://127.0.0.1:$kore_static_port/" "static asset server"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_static_port/")
  case "$body" in
    *'<title>vectis</title>'*) ;;
    *)
      printf '%s\n' "Unexpected static index response: $body" >&2
      return 1
      ;;
  esac
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_static_port/assets/app.js")
  case "$body" in
    *"console.log('vectis');"*) ;;
    *)
      printf '%s\n' "Unexpected static app response: $body" >&2
      return 1
      ;;
  esac
  status=$(curl --max-time 3 -fsS -o /dev/null -w '%{http_code}' -I \
    "http://127.0.0.1:$kore_static_port/assets/app.js")
  if [ "$status" != "200" ]; then
    printf '%s\n' "Unexpected static HEAD status: $status" >&2
    return 1
  fi
  for traversal_path in \
    "/assets/../secret" \
    "/assets/%2e%2e/secret" \
    "/assets/%2E%2E/secret" \
    "/assets/..%2fsecret"; do
    status=$(curl --path-as-is --max-time 3 -fsS -o /dev/null -w '%{http_code}' \
      "http://127.0.0.1:$kore_static_port$traversal_path" || true)
    if [ "$status" != "400" ] && [ "$status" != "404" ]; then
      printf '%s\n' "Unexpected traversal status for $traversal_path: $status" >&2
      return 1
    fi
  done
}

run_lua_examples() {
  script="$work_dir/vectis-e2e.lua"
  pack_script="$work_dir/vectis-e2e-pack.lua"
  shebang_script="$work_dir/vectis-e2e-shebang.lua"
  packed="$work_dir/vectis-e2e-packed"

  printf '[e2e] lua runner\n'
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' \
    'assert(arg[1] == "first")' \
    'assert(arg[2] == "second")' >"$script"
  "$repo_root/build/debug/vectis" "$script" first second

  printf '[e2e] lua shebang\n'
  printf '#!%s\n%s\n%s\n' \
    "$repo_root/build/debug/vectis" \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' >"$shebang_script"
  chmod 755 "$shebang_script"
  "$shebang_script"

  printf '[e2e] lua pack\n'
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'assert(vectis.status_string(vectis.OK) == "ok")' >"$pack_script"
  "$repo_root/build/debug/vectis" pack --script "$pack_script" --output "$packed"
  "$packed"

  printf '[e2e] lua libmdf example\n'
  "$repo_root/build/debug/vectis" "$repo_root/examples/lua/mdf_render.lua"
}

run_tls_cert_examples() {
  printf '[e2e] certificate generation and validation\n'
  "$repo_root/build/debug/tests/unit/vectis_unit_certs"

  printf '[e2e] https runtime with CA chain validation\n'
  "$repo_root/build/debug/tests/unit/vectis_unit_https_runtime"

  printf '[e2e] https runtime with required client certificate\n'
  "$repo_root/build/debug/tests/unit/vectis_unit_https_mtls_runtime"
}

run_kore_examples() {
  body=

  printf '[e2e] kore basic server\n'
  start_server "kore basic" "$work_dir/kore-basic.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_basic_port" \
      "$repo_root/build/debug/examples/vectis_example_kore_basic"
  wait_for_http "http://127.0.0.1:$kore_basic_port/health" "kore basic server"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_basic_port/health")
  if [ "$body" != "ok" ]; then
    printf '%s\n' "Unexpected /health response: $body" >&2
    return 1
  fi

  printf '[e2e] kore lockd api\n'
  start_server "kore lockd" "$work_dir/kore-lockd.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_lockd_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      "$repo_root/build/debug/examples/vectis_example_kore_lockd_api"
  wait_for_http "http://127.0.0.1:$kore_lockd_port/state/e2e-order" "kore lockd api"
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_lockd_port/state/e2e-order")
  case "$body" in
    *'"id":"e2e-order"'*'"status":"loaded"'*) ;;
    *)
      printf '%s\n' "Unexpected /state response: $body" >&2
      return 1
      ;;
  esac

  printf '[e2e] kore lockd workflow with consumer deferral\n'
  workflow_id=${VECTIS_E2E_WORKFLOW_ID:-"e2e-$$"}
  workflow_queue=${VECTIS_E2E_WORKFLOW_QUEUE:-"vectis-e2e-workflow-$$"}
  workflow_namespace=${VECTIS_E2E_WORKFLOW_NAMESPACE:-"examples"}
  workflow_content=${VECTIS_E2E_WORKFLOW_CONTENT:-"vectis workflow content $$"}
  workflow_consumer_first_log="$work_dir/workflow-consumer-first.log"
  workflow_consumer_second_log="$work_dir/workflow-consumer-second.log"
  start_server "kore workflow" "$work_dir/kore-workflow.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_workflow_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
      VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
      VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
      VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
      "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" server
  wait_for_http "http://127.0.0.1:$kore_workflow_port/health" "kore workflow api"
  body=$(curl --max-time 5 -fsS \
    -H 'content-type: application/json' \
    -X POST \
    --data "{\"content\":\"$workflow_content\"}" \
    "http://127.0.0.1:$kore_workflow_port/workflow/$workflow_id")
  case "$body" in
    *"\"id\":\"$workflow_id\""*"\"queue\":\"$workflow_queue\""*'"counter":1'*) ;;
    *)
      printf '%s\n' "Unexpected workflow response: $body" >&2
      return 1
      ;;
  esac
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" consumer-second \
      >"$workflow_consumer_second_log" 2>&1 &
  workflow_consumer_second_pid=$!
  server_pids="$server_pids $workflow_consumer_second_pid"
  count=0
  while ! grep -q 'workflow.consumer.not_ready' "$workflow_consumer_second_log"; do
    count=$((count + 1))
    if [ "$count" -ge 20 ]; then
      sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
      printf '%s\n' "workflow second consumer did not defer the not-ready message" >&2
      return 1
    fi
    if ! kill -0 "$workflow_consumer_second_pid" >/dev/null 2>&1; then
      sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
      printf '%s\n' "workflow second consumer exited before first deferral" >&2
      return 1
    fi
    sleep 1
  done
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" consumer-first \
      >"$workflow_consumer_first_log" 2>&1 &
  workflow_consumer_first_pid=$!
  server_pids="$server_pids $workflow_consumer_first_pid"
  if ! wait "$workflow_consumer_first_pid"; then
    sed 's/^/[workflow-consumer-first] /' "$workflow_consumer_first_log" >&2
    return 1
  fi
  if ! wait "$workflow_consumer_second_pid"; then
    sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
    return 1
  fi
  if ! grep -q 'workflow.consumer.defer' "$workflow_consumer_first_log"; then
    sed 's/^/[workflow-consumer-first] /' "$workflow_consumer_first_log" >&2
    printf '%s\n' "workflow first consumer did not defer after incrementing counter" >&2
    return 1
  fi
  if ! grep -q 'workflow.consumer.ack' "$workflow_consumer_second_log"; then
    sed 's/^/[workflow-consumer-second] /' "$workflow_consumer_second_log" >&2
    printf '%s\n' "workflow second consumer did not ack final message" >&2
    return 1
  fi
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    VECTIS_E2E_WORKFLOW_ID="$workflow_id" \
    VECTIS_E2E_WORKFLOW_QUEUE="$workflow_queue" \
    VECTIS_E2E_WORKFLOW_NAMESPACE="$workflow_namespace" \
    VECTIS_E2E_WORKFLOW_CONTENT="$workflow_content" \
    "$repo_root/build/debug/examples/vectis_example_kore_workflow_e2e" verify
}

provision_ssh_public_key() {
  if ! command -v ssh-keygen >/dev/null 2>&1 || ! command -v ssh-keyscan >/dev/null 2>&1; then
    printf '%s\n' "ssh-keygen and ssh-keyscan are required for SSH e2e coverage" >&2
    return 1
  fi
  ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -f "$ssh_memory_key"
  ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -f "$ssh_bad_host_key"
}

install_ssh_public_key() {
  "$script_dir/compose.sh" exec -T ssh-sftp /bin/sh -ec \
    'mkdir -p /config/.ssh && cat > /config/.ssh/authorized_keys && chmod 700 /config/.ssh && chmod 600 /config/.ssh/authorized_keys' \
    <"$ssh_memory_key.pub"
}

provision_ssh_known_hosts() {
  if ! ssh-keyscan -p "$ssh_port" -T 10 127.0.0.1 >"$ssh_known_hosts" 2>/dev/null; then
    printf '%s\n' "failed to scan SSH/SFTP host key" >&2
    return 1
  fi
  if [ ! -s "$ssh_known_hosts" ]; then
    printf '%s\n' "ssh-keyscan returned an empty known_hosts file" >&2
    return 1
  fi
  printf '[127.0.0.1]:%s %s\n' "$ssh_port" "$(cat "$ssh_bad_host_key.pub")" >"$ssh_bad_known_hosts"
}

"$script_dir/dev-reset.sh"
provision_ssh_public_key
"$script_dir/dev-up.sh"
install_ssh_public_key
provision_ssh_known_hosts
make -C "$repo_root" build-debug

cd "$work_dir"
run_lua_examples
run_tls_cert_examples
run_lockd_examples "$disk_endpoint" disk
run_lockd_examples "$s3_endpoint" s3
run_lockd_lua_examples "$disk_endpoint" disk
run_lockd_lua_examples "$s3_endpoint" s3
run_lockd_failure_examples
run_service_examples
run_downstream_http_examples
run_static_asset_examples
run_kore_examples
