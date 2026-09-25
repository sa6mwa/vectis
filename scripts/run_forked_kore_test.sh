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
group_pid=

# Vectis puts the Kore parent and workers in a second process group. The
# private run directory records its group leader; require a member from this
# executable and our held session before signaling that group.
owned_kore_group_member() {
  local kore_group=$1
  local stat_path stat_line stat_rest state parent process_group session pid exe

  for stat_path in /proc/[0-9]*/stat; do
    [[ -r $stat_path ]] || continue
    IFS= read -r stat_line < "$stat_path" || continue
    stat_rest=${stat_line##*) }
    read -r state parent process_group session _ <<< "$stat_rest"
    [[ $process_group == "$kore_group" && $session == "$group_pid" ]] || continue
    pid=${stat_path#/proc/}
    pid=${pid%/stat}
    exe=$(readlink -f -- "/proc/$pid/exe" 2>/dev/null) || continue
    [[ $exe == "$test_executable" ]] && return 0
  done
  return 1
}

stop_kore_group() {
  local kore_group attempt

  [[ -f $run_dir/kore.pid ]] || return 0
  IFS= read -r kore_group < "$run_dir/kore.pid" || return 1
  if [[ ! $kore_group =~ ^[0-9]+$ || $kore_group == "$group_pid" ]]; then
    printf 'invalid Kore process group in %s\n' "$run_dir/kore.pid" >&2
    return 1
  fi
  if ! owned_kore_group_member "$kore_group"; then
    return 0
  fi
  printf 'stopping owned Kore process group %s\n' "$kore_group" >&2
  kill -TERM -- "-$kore_group" 2>/dev/null || true
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! kill -0 -- "-$kore_group" 2>/dev/null; then
      return 0
    fi
    sleep 0.02
  done
  if owned_kore_group_member "$kore_group"; then
    kill -KILL -- "-$kore_group" 2>/dev/null || true
  fi
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! kill -0 -- "-$kore_group" 2>/dev/null; then
      return 0
    fi
    sleep 0.02
  done
  if owned_kore_group_member "$kore_group"; then
    printf 'owned Kore process group %s did not stop\n' "$kore_group" >&2
    return 1
  fi
  return 0
}

stop_test_group() {
  local attempt

  if [[ ! $group_pid =~ ^[0-9]+$ ]] || ! kill -0 -- "$group_pid" 2>/dev/null; then
    printf 'forked Kore test group leader is missing\n' >&2
    return 1
  fi
  kill -TERM -- "-$group_pid" 2>/dev/null || true
  if [[ -n $runner_pid ]]; then
    wait "$runner_pid" 2>/dev/null || true
    runner_pid=
  fi
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! kill -0 -- "-$group_pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.02
  done
  kill -KILL -- "-$group_pid" 2>/dev/null || true
  for (( attempt = 0; attempt < 50; attempt++ )); do
    if ! kill -0 -- "-$group_pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.02
  done
  printf 'forked Kore test group %s did not stop\n' "$group_pid" >&2
  return 1
}

finish() {
  local result=$? report

  trap - EXIT
  if (( expected_exit != 0 )) && [[ ! -f $run_dir/kore.pid ]]; then
    printf 'expected failure did not leave a Kore pid file to verify\n' >&2
    result=125
  fi
  if [[ -z $group_pid && -f $run_dir/group.pid ]]; then
    IFS= read -r group_pid < "$run_dir/group.pid" || true
  fi
  if [[ -n $group_pid ]]; then
    stop_kore_group || result=125
    stop_test_group || result=125
  else
    printf 'forked Kore test did not report its process group\n' >&2
    result=125
  fi
  if [[ -n $runner_pid ]]; then
    wait "$runner_pid" 2>/dev/null || true
  fi
  for report in "$run_dir"/asan.* "$run_dir"/ubsan.*; do
    [[ -f $report ]] || continue
    printf 'forked Kore worker sanitizer report: %s\n' "$report" >&2
    cat -- "$report" >&2
    result=1
  done
  rm -rf -- "$run_dir"
  exit "$result"
}

trap finish EXIT
trap 'exit 143' TERM INT
(
  cd "$run_dir" || exit 2
  export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$run_dir/asan"
  export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$run_dir/ubsan"
  # Keep the session leader alive after the test exits so its ID cannot be
  # reused while the wrapper tears down Kore's separate process group.
  exec setsid bash -c '
    printf "%s\n" "$BASHPID" > group.pid
    timeout --foreground -k 2s "${1}s" "$2"
    result=$?
    printf "%s\n" "$result" > exit.status.tmp
    mv -- exit.status.tmp exit.status
    while :; do sleep 60; done
  ' bash "$timeout_seconds" "$test_executable"
) &
runner_pid=$!
for (( attempt = 0; attempt < (timeout_seconds + 5) * 50; attempt++ )); do
  if [[ -f $run_dir/group.pid ]]; then
    IFS= read -r group_pid < "$run_dir/group.pid" || true
  fi
  if [[ -f $run_dir/exit.status ]]; then
    IFS= read -r actual_exit < "$run_dir/exit.status" || true
    break
  fi
  if [[ -n $group_pid ]] && ! kill -0 -- "$group_pid" 2>/dev/null; then
    printf 'forked Kore test group exited before reporting status\n' >&2
    exit 125
  fi
  sleep 0.02
done
if [[ ! ${actual_exit-} =~ ^[0-9]+$ ]]; then
  printf 'forked Kore test did not report an exit status\n' >&2
  exit 125
fi
if (( actual_exit != expected_exit )); then
  printf 'forked Kore test exited %d, expected %d\n' \
    "$actual_exit" "$expected_exit" >&2
  exit 1
fi
