#!/usr/bin/env bash
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo_root/build/dependency-cache-tests"
work=$(mktemp -d "$repo_root/build/dependency-cache-tests/cache fixture.XXXXXXXX")
origin_pid=
cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n "$origin_pid" ]]; then
    kill "$origin_pid" 2>/dev/null || true
    wait "$origin_pid" 2>/dev/null || true
  fi
  if ((status)); then printf 'Cache fixture evidence: %s\n' "$work" >&2
  else rm -rf -- "$work"; fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { printf 'Dependency cache: %s\n' "$*" >&2; exit 1; }
fixture="$work/checkout"
mkdir -p "$fixture/scripts" "$fixture/cmake" "$fixture/build/cache" "$work/assets" "$work/bin"
cp "$repo_root/scripts/"{deps,require-host-bash,cpkt-archive-cache}.sh "$fixture/scripts/"
cp "$repo_root/scripts/"{build_lua_rock,stage_lua_rock_sources}.sh "$fixture/scripts/"
cp "$repo_root/cmake/vectis_owned_paths.cmake" "$fixture/cmake/"
cat >"$fixture/scripts/cpkt-toolchains.sh" <<'TOOLS'
#!/usr/bin/env bash
[[ "$1" == ensure ]] || exit 2
printf 'status=ready\n'
TOOLS
chmod +x "$fixture/scripts/cpkt-toolchains.sh"
: >"$fixture/scripts/dependency-pins.tsv"
for name in cpkt cpktdb cpktmisc; do
  mkdir -p "$work/input/$name/include"
  printf '%s\n' "$name" >"$work/input/$name/include/fixture.h"
  chmod 444 "$work/input/$name/include/fixture.h"
  tar -czf "$work/assets/$name.tar.gz" -C "$work/input" "$name"
  digest=$(sha256sum "$work/assets/$name.tar.gz" | cut -d' ' -f1)
  printf 'sdk\t%s\t1.0.0\tx86_64-linux-gnu\t%s\thttps://fixture.invalid/%s.tar.gz\n' \
    "$name" "$digest" "$name" >>"$fixture/scripts/dependency-pins.tsv"
done
mkdir -p "$work/input/lua/lua"
printf 'return {}\n' >"$work/input/lua/lua/fixture.lua"
tar -czf "$work/assets/lua.tar.gz" -C "$work/input" lua
digest=$(sha256sum "$work/assets/lua.tar.gz" | cut -d' ' -f1)
printf 'lua\tfixture\t1.0.0\tall\t%s\thttps://fixture.invalid/lua.tar.gz\n' "$digest" >>"$fixture/scripts/dependency-pins.tsv"
printf '/* header fixture */\n' | gzip -n >"$work/assets/header.gz"
digest=$(sha256sum "$work/assets/header.gz" | cut -d' ' -f1)
printf 'header\tlibpid0\t1.0.0\tlinux\t%s\thttps://fixture.invalid/header.gz\n' "$digest" >>"$fixture/scripts/dependency-pins.tsv"

# Python supplies static fixture bytes only; Bash owns assertions and execution.
mkfifo "$work/ready"
python3 -u -m http.server 0 --bind 127.0.0.1 --directory "$work/assets" >"$work/ready" 2>"$work/requests.log" &
origin_pid=$!
read -r readiness <"$work/ready"
[[ "$readiness" =~ port\ ([0-9]+) ]] || fail "missing origin readiness: $readiness"
export ARCHIVE_ORIGIN="http://127.0.0.1:${BASH_REMATCH[1]}"
export REAL_CURL
REAL_CURL=$(command -v curl)
cat >"$work/bin/curl" <<'CURL'
#!/usr/bin/env bash
set -euo pipefail
args=()
for arg; do
  if [[ "$arg" == https://fixture.invalid/* ]]; then arg="$ARCHIVE_ORIGIN/${arg##*/}"; fi
  args+=("$arg")
done
exec "$REAL_CURL" "${args[@]}"
CURL
chmod +x "$work/bin/curl"
export PATH="$work/bin:$PATH"
export CPKT_DEPENDENCY_CACHE="$fixture/build/cache" CPKT_TOOLCHAIN_CACHE="$fixture/build/tools"
run() { bash "$fixture/scripts/deps.sh" deps-x86_64-linux-gnu >"$work/deps.out" 2>"$work/deps.err"; }
requests() { grep -c '"GET /' "$work/requests.log" || true; }
legacy="$fixture/.cache/deps/x86_64-linux-gnu"
mkdir -p "$legacy/include" "$legacy/lib" "$legacy/share/c.pkt.systems" "$legacy/bin" "$legacy/unrelated"
printf 'legacy\n' >"$legacy/share/c.pkt.systems/manifest.txt"
printf 'preserve\n' >"$legacy/unrelated/value"
run
[[ $(requests) == 5 ]] || fail 'miss did not request exactly five assets'
for leaf in include lib share bin; do
  [[ ! -e "$legacy/$leaf" ]] || fail "legacy merged $leaf survived cutover"
done
[[ $(cat "$legacy/unrelated/value") == preserve ]] || fail 'cutover removed unrelated generated state'
for name in cpkt cpktdb cpktmisc; do
  prefix="$fixture/.cache/deps/x86_64-linux-gnu/$name"
  [[ $(cat "$prefix/include/fixture.h") == "$name" ]] || fail 'provider prefixes were merged'
  [[ $(stat -c %a "$prefix/include/fixture.h") == 444 ]] || fail 'extraction changed packaged mode'
done
prefix="$fixture/.cache/deps/x86_64-linux-gnu"
[[ -f "$prefix/sources/fixture/lua/fixture.lua" && -f "$prefix/libpid0/include/libpid0.h" ]] || fail 'Lua/header selection missing'
inode=$(stat -c %i "$prefix/cpkt/include/fixture.h")
run
[[ $(requests) == 5 && $(stat -c %i "$prefix/cpkt/include/fixture.h") == "$inode" ]] || fail 'hit requested or re-extracted assets'
core_hash=$(awk -F '\t' '$2=="cpkt" {print $5}' "$fixture/scripts/dependency-pins.tsv")
printf 'corrupt' >"$CPKT_DEPENDENCY_CACHE/archives/sha256/$core_hash/cpkt.tar.gz"
run
[[ $(requests) == 6 ]] || fail 'corrupt archive was not reacquired exactly once'
cmp "$work/assets/cpkt.tar.gz" "$CPKT_DEPENDENCY_CACHE/archives/sha256/$core_hash/cpkt.tar.gz"
sed -i 's@https://fixture.invalid/cpkt.tar.gz@https://fixture.invalid/renamed-core.tar.gz@' "$fixture/scripts/dependency-pins.tsv"
run
[[ $(requests) == 6 ]] || fail 'same digest under a new asset name made a network request'

mkdir -p "$work/foreign"
mv "$prefix/cpkt" "$work/foreign/cpkt"
ln -s "$work/foreign/cpkt" "$prefix/cpkt"
if run; then fail 'borrowed provider prefix was accepted'; fi
grep -q 'cannot borrow another checkout' "$work/deps.err" || fail 'wrong prefix rejection'
rm "$prefix/cpkt"
mv "$work/foreign/cpkt" "$prefix/cpkt"
ln -s "$work/foreign" "$fixture/build/foreign-cache"
if CPKT_TOOLCHAIN_CACHE="$fixture/build/foreign-cache/tools" run; then fail 'foreign toolchain cache accepted'; fi
grep -q 'cannot borrow another checkout' "$work/deps.err" || fail 'wrong toolchain rejection'
if CPKT_DEPENDENCY_CACHE="$fixture/build/foreign-cache/deps" run; then fail 'foreign archive cache accepted'; fi
[[ ! -e "$work/foreign/tools" && ! -e "$work/foreign/deps" ]] || fail 'rejected input mutated foreign state'
for destination in "$work/foreign/rock" "$fixture/build/foreign-cache/rock"; do
  if VECTIS_VERSION=0.0.0 VECTIS_LUAROCKS_BUILD_ROOT="$destination" \
      bash "$fixture/scripts/build_lua_rock.sh" >"$work/rock.out" 2>"$work/rock.err"; then
    fail 'borrowed Lua build root accepted'
  fi
  grep -q 'cannot borrow another checkout' "$work/rock.err" || fail 'wrong Lua build ownership rejection'
done
if VECTIS_VERSION=0.0.0 VECTIS_DIST_DIR="$work/foreign/dist" \
    bash "$fixture/scripts/stage_lua_rock_sources.sh" >"$work/rock.out" 2>"$work/rock.err"; then
  fail 'borrowed Lua artifact output accepted'
fi
grep -q 'cannot borrow another checkout' "$work/rock.err" || fail 'wrong Lua output ownership rejection'
[[ ! -e "$work/foreign/rock" && ! -e "$work/foreign/dist" ]] || fail 'Lua packaging mutated rejected state'

cp "$prefix/manifest.txt" "$work/manifest.before"
# A bad newly selected digest cannot publish an archive or replace good state.
bad_hash=$(printf '%064d' 0)
sed -i "s/$core_hash/$bad_hash/;s@renamed-core.tar.gz@cpkt.tar.gz@" "$fixture/scripts/dependency-pins.tsv"
if run; then fail 'bad checksum accepted'; fi
[[ $(requests) == 7 ]] || fail 'bad digest did not make exactly one request'
grep -q 'checksum mismatch' "$work/deps.err" || fail 'wrong checksum rejection'
[[ ! -e "$CPKT_DEPENDENCY_CACHE/archives/sha256/$bad_hash/cpkt.tar.gz" ]] || fail 'bad archive published'
cmp "$work/manifest.before" "$prefix/manifest.txt"
[[ $(stat -c %i "$prefix/cpkt/include/fixture.h") == "$inode" ]] || fail 'failed acquisition replaced valid extraction'
if find "$fixture" -name '.extract-*' -o -name '.manifest.*' -o -name '.cpkt.tar.gz.*' | grep -q .; then
  fail 'failed acquisition left temporary publication state'
fi
printf 'Dependency miss/hit/corruption/identity/ownership checks passed; seven local requests.\n'
