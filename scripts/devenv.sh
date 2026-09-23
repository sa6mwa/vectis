#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
state_root="$repo_root/build/devenv"
name="vectis-$(printf %s "$repo_root" | sha256sum | cut -c1-10)"
action=${1:-}
shift || true

if [ "$action" = env ]; then
  exec python3 "$script_dir/render-devenv.py" env
fi

if ! command -v podman >/dev/null 2>&1; then
  printf '%s\n' 'Rootless Podman is required for the Vectis local environment.' >&2
  exit 1
fi
if [ "$(podman info --format '{{.Host.Security.Rootless}}')" != true ]; then
  printf '%s\n' 'Vectis local services require rootless Podman.' >&2
  exit 1
fi

pod_exists() { podman pod exists "$name-$1"; }
diagnostics() {
  printf 'Manifest: %s\n' "$state_root/devenv.yaml" >&2
  podman pod ps --filter "name=$name" >&2 || true
  for service in minio lockd ssh mqtt; do
    if pod_exists "$service"; then
      podman pod logs --tail 50 "$name-$service" >&2 || true
    fi
  done
}

case "$action" in
  render)
    python3 "$script_dir/render-devenv.py" render
    ;;
  up)
    eval "$(python3 "$script_dir/render-devenv.py" env)"
    python3 "$script_dir/render-devenv.py" render
    for service in minio lockd ssh mqtt; do
      if ! pod_exists "$service"; then
        case "$service" in
          lockd|ssh) userns='keep-id:uid=0,gid=0' ;;
          *) userns='keep-id:uid=1000,gid=1000' ;;
        esac
        if ! podman kube play --userns "$userns" "$state_root/$service.yaml"; then
          diagnostics
          exit 1
        fi
      fi
    done
    wait_for_tcp() {
      local label=$1 port=$2 count=0
      until (exec 3<>"/dev/tcp/127.0.0.1/$port") >/dev/null 2>&1; do
        count=$((count + 1))
        if [ "$count" -ge 90 ]; then
          printf 'Timed out waiting for %s at 127.0.0.1:%s\n' "$label" "$port" >&2
          diagnostics
          return 1
        fi
        sleep 1
      done
    }
    wait_for_http() {
      local count=0
      until curl --max-time 2 -fsS "http://127.0.0.1:$VECTIS_MINIO_API_PORT/minio/health/live" >/dev/null 2>&1; do
        count=$((count + 1))
        if [ "$count" -ge 90 ]; then
          printf 'Timed out waiting for MinIO at 127.0.0.1:%s\n' "$VECTIS_MINIO_API_PORT" >&2
          diagnostics
          return 1
        fi
        sleep 1
      done
    }
    wait_for_http
    for bundle in ca client disk-server s3-server; do
      count=0
      until [ -s "$state_root/state/lockd-config/$bundle.pem" ]; do
        count=$((count + 1))
        if [ "$count" -ge 90 ]; then
          printf 'Timed out waiting for %s.pem\n' "$bundle" >&2
          diagnostics
          exit 1
        fi
        sleep 1
      done
    done
    wait_for_tcp 'lockd disk' "$VECTIS_LOCKD_DISK_PORT"
    wait_for_tcp 'lockd S3' "$VECTIS_LOCKD_S3_PORT"
    if ! command -v ssh-keyscan >/dev/null 2>&1; then
      printf 'ssh-keyscan is required to verify the SSH service.\n' >&2
      exit 1
    fi
    count=0
    until [ -n "$(ssh-keyscan -T 3 -p "$VECTIS_SSH_PORT" 127.0.0.1 2>/dev/null || true)" ]; do
      count=$((count + 1))
      if [ "$count" -ge 30 ]; then
        printf 'Timed out waiting for SSH keys on 127.0.0.1:%s\n' "$VECTIS_SSH_PORT" >&2
        diagnostics
        exit 1
      fi
      sleep 1
    done
    wait_for_tcp MQTT "$VECTIS_MQTT_PORT"
    printf 'Vectis services ready.\nManifest: %s\nState: %s/state\n' \
      "$state_root/devenv.yaml" "$state_root"
    printf 'Lockd client bundle: %s/state/lockd-config/client.pem\n' "$state_root"
    printf 'SSH password: %s/credentials/ssh_password\nMinIO password: %s/credentials/minio_password\n' \
      "$state_root" "$state_root"
    printf 'MinIO: http://127.0.0.1:%s (console %s)\n' "$VECTIS_MINIO_API_PORT" "$VECTIS_MINIO_CONSOLE_PORT"
    printf 'Lockd disk: https://127.0.0.1:%s\nLockd S3: https://127.0.0.1:%s\n' "$VECTIS_LOCKD_DISK_PORT" "$VECTIS_LOCKD_S3_PORT"
    printf 'SSH/SFTP: 127.0.0.1:%s\nMQTT: 127.0.0.1:%s\n' "$VECTIS_SSH_PORT" "$VECTIS_MQTT_PORT"
    ;;
  down)
    status=0
    for service in mqtt ssh lockd minio; do
      if [ -f "$state_root/$service.yaml" ] && pod_exists "$service"; then
        if ! podman kube down "$state_root/$service.yaml"; then
          status=1
        fi
      fi
    done
    for service in mqtt ssh lockd minio; do
      if pod_exists "$service"; then
        printf 'Podman pod %s-%s still exists after devenv down.\n' "$name" "$service" >&2
        status=1
      fi
    done
    exit "$status"
    ;;
  reset)
    "$script_dir/devenv.sh" down
    rm -rf -- "$state_root"
    ;;
  ps)
    podman pod ps --filter "name=$name"
    ;;
  logs)
    for service in minio lockd ssh mqtt; do
      if pod_exists "$service"; then
        printf '\n[%s]\n' "$service"
        podman pod logs "$@" "$name-$service"
      fi
    done
    ;;
  exec-ssh)
    exec podman exec -i "$name-ssh-ssh-sftp" "$@"
    ;;
  *)
    printf 'usage: %s render|up|down|reset|ps|logs|exec-ssh|env\n' "$0" >&2
    exit 2
    ;;
esac
