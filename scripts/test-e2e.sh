#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

disk_endpoint=${VECTIS_E2E_LOCKD_DISK_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_DISK_PORT:-29441}}
s3_endpoint=${VECTIS_E2E_LOCKD_S3_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_S3_PORT:-29443}}
client_bundle=${VECTIS_E2E_LOCKD_CLIENT_BUNDLE:-$repo_root/devenv/volumes/lockd-config/client.pem}
ssh_port=${VECTIS_SSH_PORT:-29222}
mqtt_port=${VECTIS_MQTT_PORT:-21883}
default_kore_basic_port=$((31080 + ($$ % 1000) * 8))
kore_basic_port=${VECTIS_E2E_KORE_BASIC_PORT:-$default_kore_basic_port}
kore_lockd_port=${VECTIS_E2E_KORE_LOCKD_PORT:-$((kore_basic_port + 1))}
kore_workflow_port=${VECTIS_E2E_KORE_WORKFLOW_PORT:-$((kore_basic_port + 2))}
kore_downstream_port=${VECTIS_E2E_KORE_DOWNSTREAM_PORT:-$((kore_basic_port + 3))}
kore_static_port=${VECTIS_E2E_KORE_STATIC_PORT:-$((kore_basic_port + 4))}
kore_combined_port=${VECTIS_E2E_KORE_COMBINED_PORT:-$((kore_basic_port + 5))}
kore_packed_port=${VECTIS_E2E_KORE_PACKED_PORT:-$((kore_basic_port + 6))}
pack_smtp_harness=${VECTIS_E2E_PACK_SMTP_HARNESS:-$repo_root/build/debug/tests/vectis_pack_smtp_harness}
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
  packed_service_script="$work_dir/vectis-e2e-packed-service.lua"
  packed_service_site="$work_dir/vectis-e2e-packed-site"
  packed_service_cache="$work_dir/vectis-e2e-packed-cache"
  packed_service_docroot="$packed_service_cache/webdav/packed-service-e2e/content"
  packed_service_credentials="$work_dir/vectis-e2e-packed-credentials.json"
  packed_service_mailbox="$work_dir/vectis-e2e-packed-mailbox.txt"
  packed_service_enqueue="$work_dir/vectis-e2e-packed-enqueue.lua"
  packed_content_types="$work_dir/vectis-e2e-packed-content-types.json"
  shebang_script="$work_dir/vectis-e2e-shebang.lua"
  packed="$work_dir/vectis-e2e-packed"
  packed_service="$work_dir/vectis-e2e-packed-service"
  packed_service_queue="vectis-e2e-packed-$$"
  packed_service_namespace="examples"
  webdav_key_response=
  webdav_client_id=
  webdav_client_secret=
  static_put_status=
  unauth_dav_status=
  traversal_status=
  dav_write_body=
  propfind_body=
  propfind_status=
  method_status=
  guarded_status=
  password_only_status=
  wrong_token_status=
  missing_totp_status=
  wrong_totp_status=
  replay_token_status=
  blocked_email_status=
  unauth_dav_put_status=
  email_token_response=
  email_transaction_id=
  email_token=
  no_totp_key_response=
  no_totp_client_id=
  no_totp_client_secret=
  no_totp_email_token_response=
  no_totp_email_transaction_id=
  no_totp_email_token=
  expired_token_response=
  expired_transaction_id=
  expired_token=
  expired_token_status=
  expired_token_replay_status=
  mail_body=
  packed_consumer_body=

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
  "$repo_root/build/debug/vectis" -a pack --script "$pack_script" --output "$packed"
  "$packed"

  printf '[e2e] lua packed webserver\n'
  if [ ! -x "$pack_smtp_harness" ]; then
    printf '%s\n' "packed SMTP harness is missing: $pack_smtp_harness" >&2
    return 1
  fi
  mkdir -p "$packed_service_site/assets" "$packed_service_site/templates"
  printf '%s\n' \
    '<!doctype html>' \
    '<html><head><title>Vectis packed e2e</title></head>' \
    '<body><main id="app">packed service asset</main></body></html>' \
    >"$packed_service_site/index.html"
  printf '%s\n' \
    'window.vectisPackedService = true;' \
    >"$packed_service_site/app.js"
  printf '%s\n' \
    'body { color: #123456; }' \
    >"$packed_service_site/app.css"
  printf '%s\n' \
    'VX packed logo' \
    >"$packed_service_site/assets/logo.txt"
  printf '%s\n' \
    '<form id="packed-login"><input name="username"></form>' \
    >"$packed_service_site/templates/login.html"
  printf '%s\n' \
    '{"types":[' \
    '{"extension":".html","content_type":"text/html; charset=utf-8"},' \
    '{"extension":".css","content_type":"text/css"},' \
    '{"extension":".js","content_type":"application/javascript"}' \
    ']}' >"$packed_content_types"
  printf '%s\n' \
    'local vectis = require("vectis")' \
    'local port = tonumber(assert(os.getenv("VECTIS_PACKED_SERVICE_PORT")))' \
    'local credentials_path = assert(os.getenv("VECTIS_PACKED_SERVICE_CREDENTIALS"))' \
    'local cache_dir = assert(os.getenv("VECTIS_PACKED_SERVICE_CACHE"))' \
    'local smtp_url = assert(os.getenv("VECTIS_PACK_SMTP_URL"))' \
    'local lockd_endpoint = assert(os.getenv("VECTIS_PACKED_SERVICE_LOCKD_ENDPOINT"))' \
    'local lockd_queue = assert(os.getenv("VECTIS_PACKED_SERVICE_LOCKD_QUEUE"))' \
    'local lockd_namespace = os.getenv("VECTIS_PACKED_SERVICE_LOCKD_NAMESPACE") or "examples"' \
    'assert(vectis.embedded.has_assets())' \
    'local index = assert(vectis.embedded.read("/index.html"))' \
    'assert(index:match("packed service asset"))' \
    'local stat = assert(vectis.embedded.stat("/app.js"))' \
    'assert(stat.content_type == "application/javascript")' \
    'assert(assert(vectis.embedded.stat("/app.css")).content_type == "text/css")' \
    'assert(assert(vectis.embedded.stat("/assets/logo.txt")).content_type == "text/plain")' \
    'assert(assert(vectis.embedded.stat("/templates/login.html")).content_type == "text/html; charset=utf-8")' \
    'assert(assert(vectis.embedded.read("/assets/logo.txt")):match("VX packed logo"))' \
    'assert(assert(vectis.embedded.read("/templates/login.html")):match("packed%-login"))' \
    'assert(vectis.auth.store_init({ credentials_path = credentials_path }))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-user@example.com",' \
    '  password = "packed-password",' \
    '  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",' \
    '  totp_label = "Vectis:packed-user@example.com",' \
    '  issuer = "Vectis",' \
    '}))' \
    'assert(vectis.auth.user_add({' \
    '  credentials_path = credentials_path,' \
    '  username = "packed-email-only@example.com",' \
    '  password = "packed-password",' \
    '}))' \
    'local server = assert(vectis.server.new({' \
    '  app_name = "vectis-packed-service-e2e",' \
    '  bind = "127.0.0.1",' \
    '  port = port,' \
    '  tls = { mode = "disabled" },' \
    '  lockd = {' \
    '    endpoints = { lockd_endpoint },' \
    '    client_bundle = "embedded",' \
    '    namespace = lockd_namespace,' \
    '  },' \
    '}))' \
    'assert(server:static_embedded({' \
    '  path_prefix = "/site",' \
    '  cache_control = "no-store",' \
    '}))' \
    'assert(server:webdav_embedded_site({' \
    '  path_prefix = "/dav",' \
    '  cache_dir = cache_dir,' \
    '  site_id = "packed-service-e2e",' \
    '  extract_policy = "repair",' \
    '  auth = {' \
    '    kind = "native",' \
    '    credentials_path = credentials_path,' \
    '    realm = "packed-e2e",' \
    '  },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth",' \
    '  credentials_path = credentials_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Login",' \
    '  time = 59,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '  email_smtp = {' \
    '    url = smtp_url,' \
    '    mail_from = "sender@example.test",' \
    '    allowed_domain = "example.test",' \
    '    timeout_ms = 5000,' \
    '    connect_timeout_ms = 2000,' \
    '  },' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-local",' \
    '  credentials_path = credentials_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Local Token Login",' \
    '  time = 59,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_routes({' \
    '  path_prefix = "/auth-expired",' \
    '  credentials_path = credentials_path,' \
    '  realm = "packed-e2e",' \
    '  login_title = "Packed E2E Expired Login",' \
    '  time = 360,' \
    '  require_email_token = true,' \
    '  email_token_ttl_seconds = 300,' \
    '}))' \
    'assert(server:auth_json({' \
    '  path = "/api/private",' \
    '  auth = {' \
    '    kind = "native",' \
    '    credentials_path = credentials_path,' \
    '    realm = "packed-e2e",' \
    '  },' \
    '  body = [[{"ok":true,"surface":"packed-api"}]],' \
    '}))' \
    'assert(server:consumer_service({' \
    '  queue = lockd_queue,' \
    '  owner = "packed-consumer",' \
    '  name = "packed-consumer",' \
    '  worker_count = 1,' \
    '  wait_seconds = 1,' \
    '  visibility_timeout_seconds = 30,' \
    '  processing_delay_seconds = 3,' \
    '  handler = {' \
    '    kind = "webdav_marker",' \
    '    cache_dir = cache_dir,' \
    '    site_id = "packed-service-e2e",' \
    '    processing_path = "/consumer-processing.txt",' \
    '    done_path = "/consumer-done.txt",' \
    '    processing_body = "processing\n",' \
    '    done_body = "handled\n",' \
    '  },' \
    '}))' \
    'assert(server:start())' \
    'while true do' \
    '  os.execute("sleep 3600")' \
    'end' >"$packed_service_script"
  "$repo_root/build/debug/vectis" -a pack \
    --script "$packed_service_script" \
    --asset-dir "/:$packed_service_site" \
    --content-type-map "$packed_content_types" \
    --extract-mode repair \
    --lockd-bundle "$client_bundle" \
    --output "$packed_service"
  start_server "lua packed webserver" "$work_dir/lua-packed-webserver.log" \
    env VECTIS_PACKED_SERVICE_PORT="$kore_packed_port" \
      VECTIS_PACKED_SERVICE_CREDENTIALS="$packed_service_credentials" \
      VECTIS_PACKED_SERVICE_CACHE="$packed_service_cache" \
      VECTIS_PACKED_SERVICE_LOCKD_ENDPOINT="$disk_endpoint" \
      VECTIS_PACKED_SERVICE_LOCKD_QUEUE="$packed_service_queue" \
      VECTIS_PACKED_SERVICE_LOCKD_NAMESPACE="$packed_service_namespace" \
      "$pack_smtp_harness" "$packed_service" "$packed_service_mailbox"
  wait_for_http "http://127.0.0.1:$kore_packed_port/site/index.html" \
    "lua packed webserver"
  if [ ! -f "$packed_service_docroot/index.html" ] ||
      [ ! -f "$packed_service_docroot/app.css" ] ||
      [ ! -f "$packed_service_docroot/app.js" ] ||
      [ ! -f "$packed_service_docroot/assets/logo.txt" ] ||
      [ ! -f "$packed_service_docroot/templates/login.html" ]; then
    printf '%s\n' "Packed WebDAV extracted docroot was not initialized at $packed_service_docroot" >&2
    find "$packed_service_cache" -maxdepth 5 -type f -print 2>/dev/null >&2 || true
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected extracted packed index body: $body" >&2
      return 1
      ;;
  esac
  body=$(cat "$packed_service_docroot/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected extracted packed logo body: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected extracted packed template body: $body" >&2
      return 1
      ;;
  esac
  body=$(curl --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed static response: $body" >&2
      return 1
      ;;
  esac
  curl --max-time 3 -fsSI \
    "http://127.0.0.1:$kore_packed_port/site/app.js" |
    grep -qi '^content-type: application/javascript' || {
      printf '%s\n' "Packed static content type was not application/javascript" >&2
      return 1
    }
  curl --max-time 3 -fsSI \
    "http://127.0.0.1:$kore_packed_port/site/app.css" |
    grep -qi '^content-type: text/css' || {
      printf '%s\n' "Packed static CSS content type was not text/css" >&2
      return 1
    }
  curl --max-time 3 -fsSI \
    "http://127.0.0.1:$kore_packed_port/site/app.css" |
    grep -qi '^etag:' || {
      printf '%s\n' "Packed static CSS response did not include an ETag" >&2
      return 1
    }
  curl --max-time 3 -fsSI \
    "http://127.0.0.1:$kore_packed_port/site/app.css" |
    grep -qi '^cache-control: no-store' || {
      printf '%s\n' "Packed static CSS response did not include cache-control" >&2
      return 1
    }
  body=$(curl --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/app.css")
  if [ "$body" != "body { color: #123456; }" ]; then
    printf '%s\n' "Unexpected packed CSS response: $body" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected packed logo response: $body" >&2
    return 1
  fi
  curl --max-time 3 -fsSI \
    "http://127.0.0.1:$kore_packed_port/site/templates/login.html" |
    grep -qi '^content-type: text/html; charset=utf-8' || {
      printf '%s\n' "Packed template content type was not text/html; charset=utf-8" >&2
      return 1
    }
  body=$(curl --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected packed template response: $body" >&2
      return 1
      ;;
  esac
  for traversal_path in \
    "/site/../secret" \
    "/site/%2e%2e/secret" \
    "/site/%2E%2E/secret" \
    "/site/..%2fsecret"; do
    traversal_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
      "http://127.0.0.1:$kore_packed_port$traversal_path" || true)
    if [ "$traversal_status" != "400" ] && [ "$traversal_status" != "404" ]; then
      printf '%s\n' "Unexpected packed traversal status for $traversal_path: $traversal_status" >&2
      return 1
    fi
  done
  static_put_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X PUT --data 'blocked' \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  if [ "$static_put_status" = "200" ] ||
      [ "$static_put_status" = "201" ] ||
      [ "$static_put_status" = "204" ]; then
    printf '%s\n' "Unexpected packed static PUT status: $static_put_status" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS "http://127.0.0.1:$kore_packed_port/auth/login")
  case "$body" in
    *'Packed E2E Login'*'action="/auth/webdav-key"'*) ;;
    *)
      printf '%s\n' "Unexpected packed auth login response: $body" >&2
      return 1
      ;;
  esac
  password_only_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&password=packed-password&totp_code=287082' \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$password_only_status" = "200" ]; then
    printf '%s\n' "Packed auth issued WebDAV key without email token" >&2
    return 1
  fi
  no_totp_email_token_response=$(curl --max-time 3 -fsS -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  no_totp_email_transaction_id=$(printf '%s\n' "$no_totp_email_token_response" |
    sed -n 's/^transaction_id=//p')
  no_totp_email_token=$(printf '%s\n' "$no_totp_email_token_response" |
    sed -n 's/^token=//p')
  if [ -z "$no_totp_email_transaction_id" ] || [ -z "$no_totp_email_token" ]; then
    printf '%s\n' "Packed local auth did not expose a no-TOTP email token" >&2
    printf '%s\n' "$no_totp_email_token_response" >&2
    return 1
  fi
  no_totp_key_response=$(curl --max-time 3 -fsS -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$no_totp_email_transaction_id&email_token=$no_totp_email_token" \
    "http://127.0.0.1:$kore_packed_port/auth-local/webdav-key")
  no_totp_client_id=$(printf '%s\n' "$no_totp_key_response" |
    sed -n 's/^client_id=//p')
  no_totp_client_secret=$(printf '%s\n' "$no_totp_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$no_totp_client_id" ] || [ -z "$no_totp_client_secret" ]; then
    printf '%s\n' "Packed no-TOTP auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$no_totp_key_response" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS -u "$no_totp_client_id:$no_totp_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed no-TOTP guarded API response: $body" >&2
    return 1
  fi
  expired_token_response=$(curl --max-time 3 -fsS -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-email-only%40example.com&email=packed-email-only%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth-local/email-token")
  expired_transaction_id=$(printf '%s\n' "$expired_token_response" |
    sed -n 's/^transaction_id=//p')
  expired_token=$(printf '%s\n' "$expired_token_response" |
    sed -n 's/^token=//p')
  case "$expired_token_response" in
    *'expires_at=359'*) ;;
    *)
      printf '%s\n' "Packed local email token did not use expected expiry" >&2
      printf '%s\n' "$expired_token_response" >&2
      return 1
      ;;
  esac
  if [ -z "$expired_transaction_id" ] || [ -z "$expired_token" ]; then
    printf '%s\n' "Packed local auth did not expose a test email token" >&2
    printf '%s\n' "$expired_token_response" >&2
    return 1
  fi
  expired_token_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$expired_transaction_id&email_token=$expired_token" \
    "http://127.0.0.1:$kore_packed_port/auth-expired/webdav-key")
  if [ "$expired_token_status" != "401" ]; then
    printf '%s\n' "Packed auth expired email token returned unexpected status: $expired_token_status" >&2
    return 1
  fi
  expired_token_replay_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-email-only%40example.com&password=packed-password&email_transaction_id=$expired_transaction_id&email_token=$expired_token" \
    "http://127.0.0.1:$kore_packed_port/auth-expired/webdav-key")
  if [ "$expired_token_replay_status" != "401" ]; then
    printf '%s\n' "Packed auth expired email token replay returned unexpected status: $expired_token_replay_status" >&2
    return 1
  fi
  blocked_email_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40blocked.test' \
    "http://127.0.0.1:$kore_packed_port/auth/email-token")
  if [ "$blocked_email_status" != "400" ]; then
    printf '%s\n' "Packed auth allowlisted SMTP route returned unexpected blocked-email status: $blocked_email_status" >&2
    return 1
  fi
  if [ -e "$packed_service_mailbox" ]; then
    printf '%s\n' "Packed SMTP harness mailbox was written for blocked recipient" >&2
    return 1
  fi
  email_token_response=$(curl --max-time 3 -fsS -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=packed-user%40example.com&email=packed-user%40example.test' \
    "http://127.0.0.1:$kore_packed_port/auth/email-token")
  email_transaction_id=$(printf '%s\n' "$email_token_response" |
    sed -n 's/^transaction_id=//p')
  if [ -z "$email_transaction_id" ]; then
    printf '%s\n' "Packed auth did not issue an email transaction" >&2
    printf '%s\n' "$email_token_response" >&2
    return 1
  fi
  case "$email_token_response" in
    *'token='*)
      printf '%s\n' "Packed auth exposed SMTP-delivered token in HTTP response" >&2
      printf '%s\n' "$email_token_response" >&2
      return 1
      ;;
  esac
  if [ ! -s "$packed_service_mailbox" ]; then
    printf '%s\n' "Packed SMTP harness did not capture a mailbox" >&2
    return 1
  fi
  mail_body=$(cat "$packed_service_mailbox")
  case "$mail_body" in
    *'Subject: Vectis login token'*"Transaction: $email_transaction_id"*) ;;
    *)
      printf '%s\n' "Packed SMTP mailbox did not contain expected token email" >&2
      printf '%s\n' "$mail_body" >&2
      return 1
      ;;
  esac
  email_token=$(printf '%s\n' "$mail_body" |
    sed -n 's/.*Your Vectis login token is: \([^[:space:]]*\).*/\1/p')
  if [ -z "$email_token" ]; then
    printf '%s\n' "Packed SMTP mailbox did not contain a login token" >&2
    printf '%s\n' "$mail_body" >&2
    return 1
  fi
  wrong_token_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=wrong" \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$wrong_token_status" != "401" ]; then
    printf '%s\n' "Packed auth wrong email token returned unexpected status: $wrong_token_status" >&2
    return 1
  fi
  missing_totp_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$missing_totp_status" != "401" ]; then
    printf '%s\n' "Packed auth missing TOTP returned unexpected status: $missing_totp_status" >&2
    return 1
  fi
  wrong_totp_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=000000&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$wrong_totp_status" != "401" ]; then
    printf '%s\n' "Packed auth wrong TOTP returned unexpected status: $wrong_totp_status" >&2
    return 1
  fi
  webdav_key_response=$(curl --max-time 3 -fsS -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  replay_token_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X POST \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data "username=packed-user%40example.com&password=packed-password&totp_code=287082&email_transaction_id=$email_transaction_id&email_token=$email_token" \
    "http://127.0.0.1:$kore_packed_port/auth/webdav-key")
  if [ "$replay_token_status" != "401" ]; then
    printf '%s\n' "Packed auth replayed email token returned unexpected status: $replay_token_status" >&2
    return 1
  fi
  webdav_client_id=$(printf '%s\n' "$webdav_key_response" |
    sed -n 's/^client_id=//p')
  webdav_client_secret=$(printf '%s\n' "$webdav_key_response" |
    sed -n 's/^client_secret=//p')
  if [ -z "$webdav_client_id" ] || [ -z "$webdav_client_secret" ]; then
    printf '%s\n' "Packed auth did not issue WebDAV credentials" >&2
    printf '%s\n' "$webdav_key_response" >&2
    return 1
  fi
  unauth_dav_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$unauth_dav_status" = "200" ]; then
    printf '%s\n' "Packed WebDAV unexpectedly allowed unauthenticated GET" >&2
    return 1
  fi
  unauth_dav_put_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -X PUT --data 'anonymous write' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$unauth_dav_put_status" = "200" ] ||
      [ "$unauth_dav_put_status" = "201" ] ||
      [ "$unauth_dav_put_status" = "204" ]; then
    printf '%s\n' "Packed WebDAV unexpectedly allowed unauthenticated PUT: $unauth_dav_put_status" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Anonymous packed WebDAV PUT mutated extracted docroot: $body" >&2
      return 1
      ;;
  esac
  guarded_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$guarded_status" = "200" ]; then
    printf '%s\n' "Packed guarded API unexpectedly allowed anonymous GET" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/api/private")
  if [ "$body" != '{"ok":true,"surface":"packed-api"}' ]; then
    printf '%s\n' "Unexpected packed guarded API response: $body" >&2
    return 1
  fi
  printf '%s\n' \
    'local lockdc = require("lockdc")' \
    'local endpoint = assert(os.getenv("LOCKD_ENDPOINT"))' \
    'local bundle = assert(os.getenv("LOCKD_CLIENT_BUNDLE"))' \
    'local queue = assert(os.getenv("LOCKD_QUEUE"))' \
    'local namespace = assert(os.getenv("LOCKD_NAMESPACE"))' \
    'local client = assert(lockdc.open({' \
    '  endpoints = { endpoint },' \
    '  client_bundle_path = bundle,' \
    '  default_namespace = namespace,' \
    '}))' \
    'assert(client:enqueue({' \
    '  queue = queue,' \
    '  visibility_timeout_seconds = 30,' \
    '  ttl_seconds = 3600,' \
    '  max_attempts = 5,' \
    '  content_type = "text/plain",' \
    '}, "packed-consumer-message"))' \
    'client:close()' >"$packed_service_enqueue"
  env LOCKD_ENDPOINT="$disk_endpoint" \
    LOCKD_CLIENT_BUNDLE="$client_bundle" \
    LOCKD_QUEUE="$packed_service_queue" \
    LOCKD_NAMESPACE="$packed_service_namespace" \
    "$repo_root/build/debug/vectis" "$packed_service_enqueue"
  count=0
  while :; do
    packed_consumer_body=$(curl --max-time 3 -fsS \
      -u "$webdav_client_id:$webdav_client_secret" \
      "http://127.0.0.1:$kore_packed_port/dav/consumer-processing.txt" || true)
    if [ "$packed_consumer_body" = "processing" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[lua-packed-webserver] /' "$work_dir/lua-packed-webserver.log" >&2
      printf '%s\n' "Packed consumer did not enter processing state: $packed_consumer_body" >&2
      return 1
    fi
    sleep 1
  done
  method_status=$(curl --max-time 5 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'webdav during packed consumer' \
    "http://127.0.0.1:$kore_packed_port/dav/live-during-consumer.txt")
  if [ "$method_status" != "201" ] && [ "$method_status" != "204" ]; then
    printf '%s\n' "Unexpected packed WebDAV PUT status during consumer work: $method_status" >&2
    return 1
  fi
  body=$(curl --max-time 5 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/live-during-consumer.txt")
  if [ "$body" != "webdav during packed consumer" ]; then
    printf '%s\n' "Unexpected packed WebDAV body during consumer work: $body" >&2
    return 1
  fi
  count=0
  while :; do
    packed_consumer_body=$(curl --max-time 3 -fsS \
      -u "$webdav_client_id:$webdav_client_secret" \
      "http://127.0.0.1:$kore_packed_port/dav/consumer-done.txt" || true)
    if [ "$packed_consumer_body" = "handled" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[lua-packed-webserver] /' "$work_dir/lua-packed-webserver.log" >&2
      printf '%s\n' "Packed consumer did not finish: $packed_consumer_body" >&2
      return 1
    fi
    sleep 1
  done
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Unexpected packed WebDAV embedded response: $body" >&2
      return 1
      ;;
  esac
  propfind_status=$(curl --max-time 3 -sS -o "$work_dir/packed-propfind-root.xml" \
    -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PROPFIND -H 'Depth: 1' \
    "http://127.0.0.1:$kore_packed_port/dav")
  propfind_body=$(cat "$work_dir/packed-propfind-root.xml")
  if [ "$propfind_status" != "207" ]; then
    printf '%s\n' "Unexpected packed WebDAV PROPFIND status: $propfind_status" >&2
    printf '%s\n' "$propfind_body" >&2
    return 1
  fi
  case "$propfind_body" in
    *'<D:href>/dav/index.html</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list index.html: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/app.css</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list app.css: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/app.js</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list app.js: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/assets</D:href>'*|*'<D:href>/dav/assets/</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list assets directory: $propfind_body" >&2
      return 1
      ;;
  esac
  case "$propfind_body" in
    *'<D:href>/dav/templates</D:href>'*|*'<D:href>/dav/templates/</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV PROPFIND did not list templates directory: $propfind_body" >&2
      return 1
      ;;
  esac
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/assets/logo.txt")
  if [ "$body" != "VX packed logo" ]; then
    printf '%s\n' "Unexpected packed WebDAV logo response: $body" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/templates/login.html")
  case "$body" in
    *'packed-login'*'username'*) ;;
    *)
      printf '%s\n' "Unexpected packed WebDAV template response: $body" >&2
      return 1
      ;;
  esac
  curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'webdav index override' \
    "http://127.0.0.1:$kore_packed_port/dav/index.html" >/dev/null
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/index.html")
  if [ "$body" != "webdav index override" ]; then
    printf '%s\n' "Unexpected packed WebDAV index override: $body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/index.html")
  if [ "$body" != "webdav index override" ]; then
    printf '%s\n' "Packed WebDAV index override did not mutate extracted docroot: $body" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS \
    "http://127.0.0.1:$kore_packed_port/site/index.html")
  case "$body" in
    *'packed service asset'*) ;;
    *)
      printf '%s\n' "Packed static asset changed after WebDAV override: $body" >&2
      return 1
      ;;
  esac
  curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'mutable packed note' \
    "http://127.0.0.1:$kore_packed_port/dav/note.txt" >/dev/null
  dav_write_body=$(curl --max-time 3 -fsS \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/note.txt")
  if [ "$dav_write_body" != "mutable packed note" ]; then
    printf '%s\n' "Unexpected packed WebDAV write response: $dav_write_body" >&2
    return 1
  fi
  body=$(cat "$packed_service_docroot/note.txt")
  if [ "$body" != "mutable packed note" ]; then
    printf '%s\n' "Packed WebDAV write did not land in extracted docroot: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X MKCOL "http://127.0.0.1:$kore_packed_port/dav/docs")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV MKCOL status: $method_status" >&2
    return 1
  fi
  curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    -X PUT --data 'copied then moved' \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt" >/dev/null
  propfind_status=$(curl --max-time 3 -sS -o "$work_dir/packed-propfind-docs.xml" \
    -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X PROPFIND -H 'Depth: 1' \
    "http://127.0.0.1:$kore_packed_port/dav/docs")
  propfind_body=$(cat "$work_dir/packed-propfind-docs.xml")
  if [ "$propfind_status" != "207" ]; then
    printf '%s\n' "Unexpected packed WebDAV collection PROPFIND status: $propfind_status" >&2
    printf '%s\n' "$propfind_body" >&2
    return 1
  fi
  case "$propfind_body" in
    *'<D:href>/dav/docs/a.txt</D:href>'*) ;;
    *)
      printf '%s\n' "Packed WebDAV collection did not list a.txt: $propfind_body" >&2
      return 1
      ;;
  esac
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X COPY \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a.txt")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV COPY status: $method_status" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$body" != "copied then moved" ]; then
    printf '%s\n' "Unexpected packed WebDAV copied body: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X MOVE \
    -H "Destination: http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$method_status" != "201" ]; then
    printf '%s\n' "Unexpected packed WebDAV MOVE status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-copy.txt")
  if [ "$method_status" != "404" ]; then
    printf '%s\n' "Packed WebDAV MOVE left source readable: $method_status" >&2
    return 1
  fi
  body=$(curl --max-time 3 -fsS -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$body" != "copied then moved" ]; then
    printf '%s\n' "Unexpected packed WebDAV moved body: $body" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    -X DELETE "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$method_status" != "204" ]; then
    printf '%s\n' "Unexpected packed WebDAV DELETE status: $method_status" >&2
    return 1
  fi
  method_status=$(curl --max-time 3 -sS -o /dev/null -w '%{http_code}' \
    -u "$webdav_client_id:$webdav_client_secret" \
    "http://127.0.0.1:$kore_packed_port/dav/docs/a-moved.txt")
  if [ "$method_status" != "404" ]; then
    printf '%s\n' "Packed WebDAV DELETE left file readable: $method_status" >&2
    return 1
  fi

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

  printf '[e2e] kore webdav and lockd consumer in one process\n'
  combined_queue=${VECTIS_E2E_COMBINED_QUEUE:-"vectis-e2e-combined-$$"}
  combined_namespace=${VECTIS_E2E_COMBINED_NAMESPACE:-"examples"}
  combined_cache="$work_dir/combined-webdav"
  start_server "kore webdav lockd consumer" "$work_dir/kore-combined.log" \
    env VECTIS_KORE_TLS=disabled \
      VECTIS_KORE_PORT="$kore_combined_port" \
      LOCKD_ENDPOINT="$disk_endpoint" \
      LOCKD_CLIENT_BUNDLE="$client_bundle" \
      VECTIS_E2E_COMBINED_QUEUE="$combined_queue" \
      VECTIS_E2E_COMBINED_NAMESPACE="$combined_namespace" \
      VECTIS_E2E_COMBINED_WEBDAV_CACHE="$combined_cache" \
      "$repo_root/build/debug/examples/vectis_example_kore_webdav_lockd_consumer_e2e"
  wait_for_http "http://127.0.0.1:$kore_combined_port/health" "kore webdav lockd consumer"
  body=$(curl --max-time 5 -fsS -X POST --data '' \
    "http://127.0.0.1:$kore_combined_port/enqueue/e2e-combined-$$")
  if [ "$body" != "queued" ]; then
    printf '%s\n' "Unexpected combined enqueue response: $body" >&2
    return 1
  fi
  count=0
  while :; do
    body=$(curl --max-time 3 -fsS \
      "http://127.0.0.1:$kore_combined_port/dav/consumer-processing.txt" || true)
    if [ "$body" = "processing" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[kore-combined] /' "$work_dir/kore-combined.log" >&2
      printf '%s\n' "combined consumer did not enter processing state: $body" >&2
      return 1
    fi
    sleep 1
  done
  status=$(printf 'during consumer\n' | curl --max-time 5 -fsS \
    -o /dev/null -w '%{http_code}' -X PUT --data-binary @- \
    "http://127.0.0.1:$kore_combined_port/dav/live.txt")
  if [ "$status" != "201" ] && [ "$status" != "204" ]; then
    printf '%s\n' "Unexpected combined WebDAV PUT status during consumer work: $status" >&2
    return 1
  fi
  body=$(curl --max-time 5 -fsS \
    "http://127.0.0.1:$kore_combined_port/dav/live.txt")
  if [ "$body" != "during consumer" ]; then
    printf '%s\n' "Unexpected combined WebDAV body during consumer work: $body" >&2
    return 1
  fi
  count=0
  while :; do
    body=$(curl --max-time 3 -fsS \
      "http://127.0.0.1:$kore_combined_port/dav/consumer-done.txt" || true)
    if [ "$body" = "handled" ]; then
      break
    fi
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      sed 's/^/[kore-combined] /' "$work_dir/kore-combined.log" >&2
      printf '%s\n' "combined consumer did not finish: $body" >&2
      return 1
    fi
    sleep 1
  done
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
  count=0
  while ! ssh-keyscan -p "$ssh_port" -T 5 127.0.0.1 >"$ssh_known_hosts" 2>/dev/null; do
    count=$((count + 1))
    if [ "$count" -ge 30 ]; then
      printf '%s\n' "failed to scan SSH/SFTP host key" >&2
      return 1
    fi
    sleep 1
  done
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
