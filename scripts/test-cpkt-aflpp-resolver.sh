#!/usr/bin/env bash
set -euo pipefail

skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
resolver="$skill_dir/scripts/cpkt-aflpp.sh"
fail() { printf 'test-cpkt-aflpp-resolver: %s\n' "$*" >&2; exit 1; }

[[ -x "$resolver" ]] || fail 'resolver is not executable'
bash -n "$resolver"
grep -Fq 'version=5.02c' "$resolver" || fail 'AFL++ version is not pinned'
grep -Fq 'archive_sha256=' "$resolver" || fail 'AFL++ checksum is not pinned'
grep -Fq 'cpkt-toolchains.sh' "$resolver" || fail 'resolver does not use the embedded Bootlin resolver'
grep -Fq 'install_cleanup_trap -f "$dl"' "$resolver" || fail 'resolver does not clean interrupted AFL++ downloads'
grep -Fq 'install_cleanup_trap -rf "$tmp"' "$resolver" || fail 'resolver does not clean failed staging state'
grep -Fq 'with_cache_lock "$c/locks/aflplusplus-${version}-x86_64-linux-gnu.lock" ensure_locked' "$resolver" || fail 'resolver does not serialize shared AFL++ publication'
grep -Fq 'collection_id()' "$resolver" || fail 'resolver does not key AFL++ caches by Bootlin collection identity'
grep -Fq '.cpkt-aflpp-revision-$revision-$id' "$resolver" || fail 'resolver readiness marker is not tied to the Bootlin collection identity'
grep -Fq 'ready "$r" "$id" && return' "$resolver" || fail 'resolver does not recheck collection-specific AFL++ readiness under the lock'
grep -Fq -- '-x "$r/bin/afl-showmap"' "$resolver" || fail 'resolver readiness does not require afl-showmap'
grep -Fq '"-DAFL_PATH=\"$helper\""' "$resolver" || fail 'resolver does not preserve AFL++ cache paths as one compiler argument'
grep -Fq 'export PATH=%q' "$resolver" || fail 'resolver env output does not prepend the pinned AFL++ bin directory'

scratch_root="$skill_dir/build/cpkt-aflpp-tests"
mkdir -p "$scratch_root"
fake_bin=$(mktemp -d "$scratch_root/fake-bin.XXXXXX")
trap 'rm -rf "$fake_bin"' EXIT HUP INT TERM
printf '#!/bin/sh\nprintf "aarch64\\n"\n' > "$fake_bin/uname"
chmod +x "$fake_bin/uname"
if PATH="$fake_bin:$PATH" "$resolver" ensure >/dev/null 2>&1; then
  fail 'resolver accepted a non-native host'
fi

if env -u HOME -u XDG_CACHE_HOME -u CPKT_TOOLCHAIN_CACHE "$resolver" discover >/dev/null 2>&1; then
  fail 'resolver accepted a missing cache root'
fi

if "$resolver" ensure extra >/dev/null 2>&1; then
  fail 'resolver accepted an invalid command shape'
fi

signal_root=$(mktemp -d "$scratch_root/signal.XXXXXX")
trap 'rm -rf "$fake_bin" "$signal_root"' EXIT HUP INT TERM
signal_skill="$signal_root/skill"
signal_bin="$signal_root/bin"
signal_cache="$signal_root/cache"
signal_bootlin="$signal_root/bootlin"
mkdir -p "$signal_skill/scripts" "$signal_bin" "$signal_bootlin/include"
cp "$resolver" "$signal_skill/scripts/cpkt-aflpp.sh"
chmod +x "$signal_skill/scripts/cpkt-aflpp.sh"
touch "$signal_bootlin/include/gmp.h"
printf '#!/bin/sh\nexit 0\n' > "$signal_bin/cc"
printf '#!/bin/sh\nexit 0\n' > "$signal_bin/cxx"
cat > "$signal_skill/scripts/cpkt-toolchains.sh" <<EOF
#!/bin/sh
case "\$1" in
  ensure) exit 0 ;;
  discover)
    printf 'cc=%s\\n' '$signal_bin/cc'
    printf 'cxx=%s\\n' '$signal_bin/cxx'
    printf 'root=%s\\n' '$signal_bootlin'
    ;;
  *) exit 2 ;;
esac
EOF
cat > "$signal_bin/sha256sum" <<'EOF'
#!/bin/sh
cat >/dev/null
exit 0
EOF
cat > "$signal_bin/curl" <<'EOF'
#!/bin/sh
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) output=$2; shift 2 ;;
    *) shift ;;
  esac
done
mkdir -p "$(dirname -- "$output")"
: > "$output"
kill -TERM "$PPID"
EOF
chmod +x "$signal_skill/scripts/cpkt-toolchains.sh" "$signal_bin/cc" "$signal_bin/cxx" "$signal_bin/sha256sum" "$signal_bin/curl"
set +e
PATH="$signal_bin:$PATH" CPKT_TOOLCHAIN_CACHE="$signal_cache" "$signal_skill/scripts/cpkt-aflpp.sh" ensure >/dev/null 2>&1
signal_status=$?
set -e
[[ $signal_status -ne 0 ]] || fail 'resolver accepted an interrupted AFL++ download'
if find "$signal_cache/archives" -maxdepth 1 -name 'AFLplusplus-5.02c.tar.gz.tmp.*' -print -quit | grep -q .; then
  fail 'interrupted AFL++ download left a temporary archive in the shared cache'
fi

printf 'AFL++ resolver tests passed\n'
