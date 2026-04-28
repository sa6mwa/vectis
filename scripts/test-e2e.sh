#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

disk_endpoint=${VECTIS_E2E_LOCKD_DISK_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_DISK_PORT:-29441}}
s3_endpoint=${VECTIS_E2E_LOCKD_S3_ENDPOINT:-https://127.0.0.1:${VECTIS_LOCKD_S3_PORT:-29443}}
client_bundle=${VECTIS_E2E_LOCKD_CLIENT_BUNDLE:-$repo_root/devenv/volumes/lockd-config/client.pem}
ssh_port=${VECTIS_SSH_PORT:-29222}
mqtt_port=${VECTIS_MQTT_PORT:-21883}
work_dir=$(mktemp -d)

cleanup() {
  rm -rf "$work_dir"
  if [ "${VECTIS_E2E_KEEP_DEVSERVICES:-0}" != "1" ]; then
    "$script_dir/dev-down.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

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

run_service_examples() {
  printf '[e2e] mqtt publish\n'
  env VECTIS_MQTT_URL="mqtt://127.0.0.1:$mqtt_port" \
    "$repo_root/build/debug/examples/vectis_example_mqtt"

  printf '[e2e] ssh command\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    "$repo_root/build/debug/examples/vectis_example_ssh"

  printf '[e2e] libssh2 sftp upload/download\n'
  env VECTIS_SSH_HOST="127.0.0.1" \
    VECTIS_SSH_PORT="$ssh_port" \
    VECTIS_SSH_USERNAME="vectis" \
    VECTIS_SSH_PASSWORD="vectispass" \
    "$repo_root/build/debug/examples/vectis_example_ssh_sftp"

  printf '[e2e] curl sftp upload/download\n'
  env VECTIS_SFTP_URL="sftp://127.0.0.1:$ssh_port" \
    VECTIS_SFTP_USERNAME="vectis" \
    VECTIS_SFTP_PASSWORD="vectispass" \
    "$repo_root/build/debug/examples/vectis_example_sftp"
}

"$script_dir/dev-reset.sh"
"$script_dir/dev-up.sh"
make -C "$repo_root" build-debug

cd "$work_dir"
run_lockd_examples "$disk_endpoint" disk
run_lockd_examples "$s3_endpoint" s3
run_service_examples
