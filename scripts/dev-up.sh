#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
compose="$script_dir/compose.sh"

disk_port=${VECTIS_LOCKD_DISK_PORT:-29441}
s3_port=${VECTIS_LOCKD_S3_PORT:-29443}
minio_api_port=${VECTIS_MINIO_API_PORT:-29000}
minio_console_port=${VECTIS_MINIO_CONSOLE_PORT:-29001}
ssh_port=${VECTIS_SSH_PORT:-29222}
mqtt_port=${VECTIS_MQTT_PORT:-21883}

mkdir -p \
  "$repo_root/devenv/volumes/lockd-config" \
  "$repo_root/devenv/volumes/lockd-disk-storage" \
  "$repo_root/devenv/volumes/minio-data/lockd-client-s3" \
  "$repo_root/devenv/volumes/ssh-sftp-config" \
  "$repo_root/devenv/volumes/ssh-sftp-data"

wait_for_file() {
  path=$1
  label=$2
  count=0
  while [ ! -s "$path" ]; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $path" >&2
      exit 1
    fi
    sleep 1
  done
}

wait_for_tcp() {
  host=$1
  port=$2
  label=$3
  count=0
  while ! (exec 3<>"/dev/tcp/$host/$port") >/dev/null 2>&1; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $host:$port" >&2
      exit 1
    fi
    sleep 1
  done
  exec 3>&-
}

wait_for_http() {
  url=$1
  label=$2
  count=0
  while ! curl -fsS "$url" >/dev/null 2>&1; do
    count=$((count + 1))
    if [ "$count" -ge 60 ]; then
      printf '%s\n' "Timed out waiting for $label at $url" >&2
      exit 1
    fi
    sleep 1
  done
}

"$compose" up -d --remove-orphans minio
wait_for_http "http://127.0.0.1:$minio_api_port/minio/health/live" "minio"

"$compose" up -d --no-recreate \
  minio-init \
  lockd-ca \
  lockd-disk-server-cert \
  lockd-s3-server-cert \
  lockd-client-cert \
  lockd-disk \
  lockd-s3 \
  ssh-sftp \
  mqtt

wait_for_file "$repo_root/devenv/volumes/lockd-config/ca.pem" "lockd CA bundle"
wait_for_file "$repo_root/devenv/volumes/lockd-config/client.pem" "lockd client bundle"
wait_for_file "$repo_root/devenv/volumes/lockd-config/disk-server.pem" "lockd disk server bundle"
wait_for_file "$repo_root/devenv/volumes/lockd-config/s3-server.pem" "lockd s3 server bundle"
wait_for_tcp "127.0.0.1" "$disk_port" "lockd disk listener"
wait_for_tcp "127.0.0.1" "$s3_port" "lockd s3 listener"
wait_for_tcp "127.0.0.1" "$ssh_port" "ssh/sftp listener"
wait_for_tcp "127.0.0.1" "$mqtt_port" "mqtt listener"

cat <<EOF
vectis devenv is up.

Endpoints:
  lockd disk: https://127.0.0.1:$disk_port
  lockd s3:   https://127.0.0.1:$s3_port
  ssh/sftp:   127.0.0.1:$ssh_port
  mqtt:       mqtt://127.0.0.1:$mqtt_port

Generated lockd client bundle:
  ./devenv/volumes/lockd-config/client.pem

MinIO:
  API:     http://127.0.0.1:$minio_api_port
  Console: http://127.0.0.1:$minio_console_port
  Bucket:  lockd-client-s3
EOF
