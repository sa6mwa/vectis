#!/usr/bin/env bash
set -u

if (( $# != 3 )); then
  printf 'usage: %s timeout-seconds expected-exit executable\n' "$0" >&2
  exit 2
fi

timeout_seconds=$1
expected_exit=$2
test_executable=$(realpath -- "$3") || exit 2
if [[ ! $timeout_seconds =~ ^[1-9][0-9]*$ ||
      ! $expected_exit =~ ^[0-9]+$ ||
      ! -x $test_executable ]]; then
  printf 'invalid forked Kore test arguments\n' >&2
  exit 2
fi

run_dir=$(mktemp -d -- "$PWD/vectis-kore-test.XXXXXXXX") || exit 2
runner_pid=

same_executable() {
  local actual

  actual=$(readlink -f -- "/proc/$1/exe" 2>/dev/null) || return 1
  [[ $actual == "$test_executable" ]]
}

stop_exact_process() {
  local pid=$1
  local attempt

  if ! same_executable "$pid"; then
    return 0
  fi
  kill -TERM "$pid" 2>/dev/null || true
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! same_executable "$pid"; then
      return 0
    fi
    sleep 0.02
  done
  if same_executable "$pid"; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! same_executable "$pid"; then
      return 0
    fi
    sleep 0.02
  done
  printf 'forked Kore test process %s did not stop\n' "$pid" >&2
  return 1
}

finish() {
  local result=$?
  local server_pid=
  local workers=
  local worker

  trap - EXIT
  if [[ -n $runner_pid ]]; then
    kill -TERM "$runner_pid" 2>/dev/null || true
    wait "$runner_pid" 2>/dev/null || true
  fi
  if [[ -f $run_dir/kore.pid ]]; then
    IFS= read -r server_pid < "$run_dir/kore.pid" || true
    if [[ $server_pid =~ ^[0-9]+$ ]] && same_executable "$server_pid"; then
      printf 'stopping forked Kore test parent %s\n' "$server_pid" >&2
      if [[ -r /proc/$server_pid/task/$server_pid/children ]]; then
        workers=$(< "/proc/$server_pid/task/$server_pid/children")
      fi
      stop_exact_process "$server_pid" || result=125
      for worker in $workers; do
        stop_exact_process "$worker" || result=125
      done
    elif [[ $server_pid =~ ^[0-9]+$ && -e /proc/$server_pid/exe ]]; then
      printf 'refusing to stop unrelated pid %s from %s\n' \
        "$server_pid" "$run_dir/kore.pid" >&2
      result=125
    fi
  elif (( expected_exit != 0 )); then
    printf 'expected failure did not leave a Kore pid file to verify\n' >&2
    result=125
  fi
  rm -rf -- "$run_dir"
  exit "$result"
}

trap finish EXIT
trap 'exit 143' TERM INT
(
  cd "$run_dir" || exit 2
  exec timeout -k 2s "${timeout_seconds}s" "$test_executable"
) &
runner_pid=$!
wait "$runner_pid"
actual_exit=$?
runner_pid=
if (( actual_exit != expected_exit )); then
  printf 'forked Kore test exited %d, expected %d\n' \
    "$actual_exit" "$expected_exit" >&2
  exit 1
fi
