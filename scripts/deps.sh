#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-deps-host-debug}

dependency_cache="${CPKT_DEPENDENCY_CACHE:-${XDG_CACHE_HOME:-${HOME}/.cache}/c.pkt.systems/deps}"
archive_cache_root="$dependency_cache/archives/sha256"
archive_lock_root="$dependency_cache/locks"
deps_root=
target_id=
system_sha256=
lockdc_sha256=
lonejson_sha256=
pslog_sha256=
cai_sha256=
lql_sha256=
lql_lua_sha256="b440ce543586ebfc9aafd0e09a700126b9d62d85b8c34ae2ac19b0990db28438"
mdf_sha256=
softline_sha256=
target_cc=
target_ar=
target_ranlib=
target_cmake_system_name=
target_cmake_system_processor=
pid0_enabled=0

set_linux_gnu_dependency_target() {
  target_id=$1
  case "$target_id" in
    x86_64-linux-gnu)
      system_sha256="0bbb1cbaf60b0a94fb5a6b3756123088b45e2bef9e38079038f22e3c07febb2e"
      lockdc_sha256="fcce40120a8e6c6990efdb4d54c427011619abb280a8ecbd4644b720eda7cbb8"
      lonejson_sha256="e04f80b907d92f7e38f825fbd339297e85372fc1ce110abb9a93715ee450ece3"
      pslog_sha256="7981ce7e60f6f1e144042e7a9192bb661472756ae34336fb0c2ed8316b31945f"
      cai_sha256="e344102fa5b46e8c05d67a5120ea0c74bf9ee8ad9ec0bc01e08ea5ccc1f1bdc9"
      lql_sha256="a32b3ecc33b0634df23c630843b1c2c16a8a2caa947109a33bad20965e47a399"
      mdf_sha256="37ce7d4a93618b3d607de073a4f7c3518873a5d4583bbac670ee36f13e1c2ce9"
      softline_sha256="5d5e662269cf5bae9276f1ba7216dfb7e63127ea89c7c9b5f23cb33dbc970012"
      target_cmake_system_processor="x86_64"
      ;;
    aarch64-linux-gnu)
      system_sha256="3fb1fdeb83bfd58da48a3319dfc2c6d35384265b2db2074b63216240bf0fe2ad"
      lockdc_sha256="c0cf2ffb0abfd8a28613bcf40eeacd952eff72b1cfaf25c8509c626adc6a63d9"
      lonejson_sha256="d7f9c700be6f9af7e46b18d59a0be14a42bd19644a30684b5a1135f96ee2daed"
      pslog_sha256="38bb08ca6646cf186925a724b61fb534fa49ec0d5e77ca95953dd7a5b18f76e1"
      cai_sha256="654354f172f5d5194f834a6ca17f65c068960c76954018d8afffc6fd3912a9bd"
      lql_sha256="d796c3b0574cb4137c22d4fce2ef04f24ded83199e6848ae0829dc10b276cc2f"
      mdf_sha256="053416ffe7ffc2c1fcab4ad6bf13e4b1507e6822745403384b0cfb7805f73cd4"
      softline_sha256="634689c41fa6fbf6be65ae26d3c7bfbcccf7884a845e2b9edaee5a124d98c243"
      target_cmake_system_processor="aarch64"
      ;;
    armhf-linux-gnu)
      system_sha256="18738e2d8e9661ebdcc0b54f4f292f0571d04218fceb1f281e051a50928d1694"
      lockdc_sha256="b0ce4faadd3e6e4cccde78e32efced1ab7aeb595d2eb2345a629a5be921f150f"
      lonejson_sha256="3aeff1901078917a4430dc945c253cf4cec193311f35245b4ef1c62056d181c1"
      pslog_sha256="eff69fe9223cd2ad56572ad6acd768b560ac3e863e379c65367ad6338dbfffef"
      cai_sha256="82471c69ea896eb4db18fd5d7f165171575fe3c98460a9330be2e055762adebd"
      lql_sha256="fef9050c63f98f8a20d6afe9a428a172cb98a933b64221425e7eb759fb2284e6"
      mdf_sha256="b86ae3ece3af537bf382d299fa5730e33328d9401be3807f25678e7195f78be9"
      softline_sha256="69e25016671259a46bb3bc74127b3861068d96510cf5d16c971eb464552925da"
      target_cmake_system_processor="arm"
      ;;
    *)
      echo "unsupported Linux GNU dependency target: $target_id" >&2
      exit 2
      ;;
  esac
  target_cc="cpkt-toolchains:$target_id"
  target_ar="cpkt-toolchains:$target_id"
  target_ranlib="cpkt-toolchains:$target_id"
  target_cmake_system_name="Linux"
}

set_host_debug_target() {
  host_system=${VECTIS_HOST_UNAME_S:-$(uname -s)}
  host_machine=${VECTIS_HOST_UNAME_M:-$(uname -m)}

  case "$host_system:$host_machine" in
    Linux:x86_64 | Linux:amd64)
      set_linux_gnu_dependency_target "x86_64-linux-gnu"
      ;;
    Linux:aarch64 | Linux:arm64)
      set_linux_gnu_dependency_target "aarch64-linux-gnu"
      ;;
    Linux:armv6l | Linux:armv7l | Linux:armv8l)
      set_linux_gnu_dependency_target "armhf-linux-gnu"
      ;;
    *)
      echo "unsupported host for deps-host-debug: $host_system $host_machine" >&2
      echo "supported hosts: Linux x86_64, Linux aarch64, Linux armv6l/armv7l/armv8l" >&2
      exit 2
      ;;
  esac
}

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    set_host_debug_target
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    set_linux_gnu_dependency_target "x86_64-linux-gnu"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    target_id="x86_64-linux-musl"
    system_sha256="e867e7d8649bba6d6c4bed254f3a666faa090f8ccb31a3eb10b1323b694f2f21"
    lockdc_sha256="91a1fd05abf7c9c1b8ba3ad3b5891f6ba8bb85a271ebda9267338917aae92b2d"
    lonejson_sha256="ca0811bd920f6cf59f82d45e04525b562bba238564e5c5b9a00aa18331b5a5ca"
    pslog_sha256="d05e59e8d88018a2e78e0941d2db211f3c08e4fd7539065ed2de79ce7e371055"
    cai_sha256="8b3559da161f348892c3e96842933ef061b3e9d7ca50d6338b9b89822476d8ff"
    lql_sha256="6a90dd82d5d12281a2afd05025a8eba179bc775cf015da9ee3359ab50f6adcfa"
    mdf_sha256="cd6b7497e2a43cda1cd8606a0dfb796ad8b2b2ff8a49100a5e2b2a4507dc8c70"
    softline_sha256="1bdd76c953abc7994fd05c7561d0a11fae7c6d568bd263ac367e7e6ad5aa166a"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    set_linux_gnu_dependency_target "aarch64-linux-gnu"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    target_id="aarch64-linux-musl"
    system_sha256="a915993c294e96c9a84b072bed45384c23f0058c9e18cd5caeba344aaa9b5d39"
    lockdc_sha256="aac4c2cc5fdeb32a31ac44dc1960a523b9d7e5ac18dfc2e34808c5216b77fe45"
    lonejson_sha256="813950b50620cfa48e01ae0c5b30ae338b79066e750c45cefeb9a86076466903"
    pslog_sha256="fce3c4f95b317563427437313ef2eb1987dc43973b0b0bf5169763d0a2705f69"
    cai_sha256="e61042c0520774a45b924f1e03999cc062797c530e353c04110d3e25962c37cf"
    lql_sha256="b128e35e19267e6406c13831650be2cb5a20579cf142ac9beb9903705dc4ae5a"
    mdf_sha256="4725396cda74282bb90a31a0a06eb362c5407c9d66c23ddd33b387149081c7fa"
    softline_sha256="b08078b606de8582ea9e369f029a3efcb8b9a5e04a28b566ec9ca7adf7229219"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    set_linux_gnu_dependency_target "armhf-linux-gnu"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    target_id="armhf-linux-musl"
    system_sha256="7f5365014ef2222cb95c08525c0b123afb30b4f220f4edcd669f354a9af4ccab"
    lockdc_sha256="03488de7b344ec17eba33cbf23c090c6a0a8af17fda64159621a5f80677a8a3c"
    lonejson_sha256="37ba738c675b41c563b1b03ea322ad1e65dbe76749ebfd294809b50abafb2d32"
    pslog_sha256="19eeadacfb82b7eba4187b1fc405225bf85a8866ea81939e2eaa841a23d3785c"
    cai_sha256="fef8d1cdb72e04583505380733daf4cbf82c3fb8f88b5051ea04d08811b5865b"
    lql_sha256="af2b00cb119834bfeb5f80c410e02338e6a51bee058b80e2b70d7ede39c42ba0"
    mdf_sha256="889786f8332e3ba0a86d2cf31a47229efe374d1d470cd3b86d193e5c5379297d"
    softline_sha256="12e1b677913f2811d627ffc445fa14c7af7518a2ac48dae98dea133437365946"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="8bc25d47d30cb40b24eb5d07c2aad7850150fdea680eccadd1c819ce945901af"
    lockdc_sha256="f34e9c94cc834e4fc276fb14ad5024c550a41e9f562c3d4e4fb2dbb6f37bbca2"
    lonejson_sha256="b351df4221e16d62b7b86940a6a6a6a4d38fffb850b2d118fca2f9f5a9bb5488"
    pslog_sha256="ff5d2106bcbc5ea5bce8dfdbca54d21650f350e50fd214a4b52ac65b4f834073"
    cai_sha256="af2069167c403c5da6c8aef40a700dcc4ef03db7b10cd83177a85dd302e52f25"
    lql_sha256="2e01c19a9ee0a12bc8e6a1411d8f048a9e2f57fe336b28332ec0fc6307665725"
    mdf_sha256="85757b37735b14e26abf46f9dfb1f7a41afbb04f6f7251d13a605e3c0c79a97d"
    softline_sha256="31e1602e059d94e1ab9acf2504b4766d0a53f21bcbb94ebdf2c297d617feef2a"
    if [ -n "${OSXCROSS_ROOT:-}" ]; then
      osxcross_root=$OSXCROSS_ROOT
    else
      osxcross_root="${HOME:-}/.local/cross/osxcross"
    fi
    osxcross_host="${VECTIS_OSXCROSS_HOST:-${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}}"
    target_cc="${CC:-$osxcross_root/bin/$osxcross_host-clang}"
    target_ar="${AR:-$osxcross_root/bin/$osxcross_host-ar}"
    target_ranlib="${RANLIB:-$osxcross_root/bin/$osxcross_host-ranlib}"
    target_cmake_system_name="Darwin"
    target_cmake_system_processor="arm64"
    ;;
  *)
    echo "usage: scripts/deps.sh [deps-host-debug|deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl|deps-arm64-apple-darwin]" >&2
    exit 2
    ;;
esac

case "$target_cmake_system_name" in
  Linux) pid0_enabled=1 ;;
  *) pid0_enabled=0 ;;
esac

if [ "${VECTIS_DEPS_DRY_RUN:-0}" = "1" ]; then
  cat <<EOF
preset=$preset
dependency_cache=$dependency_cache
deps_root=$deps_root
target_id=$target_id
target_cc=$target_cc
target_ar=$target_ar
target_ranlib=$target_ranlib
target_cmake_system_name=$target_cmake_system_name
target_cmake_system_processor=$target_cmake_system_processor
system_sha256=$system_sha256
liblockdc_sha256=$lockdc_sha256
lonejson_sha256=$lonejson_sha256
pslog_sha256=$pslog_sha256
cai_sha256=$cai_sha256
lql_sha256=$lql_sha256
lql_lua_sha256=$lql_lua_sha256
mdf_sha256=$mdf_sha256
softline_sha256=$softline_sha256
libpid0_enabled=$pid0_enabled
EOF
  if [ "$pid0_enabled" -eq 1 ]; then
    cat <<EOF
libpid0_version=0.4.2
libpid0_sha256=907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8
EOF
  fi
  exit 0
fi

mkdir -p "$archive_cache_root" "$archive_lock_root" "$deps_root/include" "$deps_root/lib"

system_version="0.9.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$archive_cache_root/$system_sha256/$system_archive"
lockdc_version="0.13.1"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$archive_cache_root/$lockdc_sha256/$lockdc_archive"
lockdc_lua_archive="liblockdc-lua-${lockdc_version}.tar.gz"
lockdc_lua_payload=""
lockdc_lua_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_lua_archive}"
lockdc_lua_sha256="62574d7d6d8bd6f87b5b54679ac767522d416aa729087e4417730c3abc10dbd1"
lockdc_lua_download="$archive_cache_root/$lockdc_lua_sha256/$lockdc_lua_archive"
lockdc_lua_source_dir="$deps_root/share/lockdc-source"
lonejson_version="0.42.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$archive_cache_root/$lonejson_sha256/$lonejson_archive"
lonejson_lua_archive="lonejson-lua-${lonejson_version}.tar.gz"
lonejson_lua_payload=""
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_sha256="8d6d25eb6cbd46eafc7e2ee555d8bde1b0dee435a97a3882b93a7533ba6b2ee4"
lonejson_lua_download="$archive_cache_root/$lonejson_lua_sha256/$lonejson_lua_archive"
lonejson_source_dir="$deps_root/share/lonejson-source"
pslog_version="0.9.0"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$archive_cache_root/$pslog_sha256/$pslog_archive"
pslog_lua_archive="lua-pslog-${pslog_version}.tar.gz"
pslog_lua_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_lua_archive}"
pslog_lua_sha256="5a73e94395ec484ff7c404651a892297dfd1e863bc361490fe95f2b7f9c5db71"
pslog_lua_download="$archive_cache_root/$pslog_lua_sha256/$pslog_lua_archive"
pslog_lua_source_dir="$deps_root/share/pslog-lua-source"
cai_version="0.3.0"
cai_archive="cai-${cai_version}-${target_id}.tar.gz"
cai_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_archive}"
cai_download="$archive_cache_root/$cai_sha256/$cai_archive"
cai_lua_archive="cai-lua-${cai_version}.tar.gz"
cai_lua_payload=""
cai_lua_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_lua_archive}"
cai_lua_sha256="ce32c6ee1bd7586112e219ba1eceac69980fabaf846e45bbe06a9e656ecd6173"
cai_lua_download="$archive_cache_root/$cai_lua_sha256/$cai_lua_archive"
cai_lua_source_dir="$deps_root/share/cai-lua-source"
lql_version="0.2.0"
lql_archive="liblql-${lql_version}-${target_id}.tar.gz"
lql_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_archive}"
lql_download="$archive_cache_root/$lql_sha256/$lql_archive"
lql_lua_archive="liblql-lua-${lql_version}.tar.gz"
lql_lua_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_lua_archive}"
lql_lua_download="$archive_cache_root/$lql_lua_sha256/$lql_lua_archive"
lql_lua_source_dir="$deps_root/share/liblql-lua-source"
mdf_version="0.6.0"
mdf_archive="libmdf-${mdf_version}-${target_id}.tar.gz"
mdf_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_archive}"
mdf_download="$archive_cache_root/$mdf_sha256/$mdf_archive"
mdf_lua_archive="libmdf-lua-${mdf_version}.tar.gz"
mdf_lua_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_lua_archive}"
mdf_lua_sha256="7541650d2aa1f29a290ce16512a37db81924cb1b63d860338f1790ae9b2326e4"
mdf_lua_download="$archive_cache_root/$mdf_lua_sha256/$mdf_lua_archive"
mdf_lua_source_dir="$deps_root/share/libmdf-lua-source"
softline_version="0.2.0"
softline_archive="softline-${softline_version}-${target_id}.tar.gz"
softline_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_archive}"
softline_download="$archive_cache_root/$softline_sha256/$softline_archive"
softline_lua_archive="softline-lua-${softline_version}.tar.gz"
softline_lua_payload=""
softline_lua_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_lua_archive}"
softline_lua_sha256="5c41eccb027546a8fa5008e883a8d6794bac0f85c75e3319d47ad845fe2b691e"
softline_lua_download="$archive_cache_root/$softline_lua_sha256/$softline_lua_archive"
softline_lua_source_dir="$deps_root/share/softline-lua-source"
pid0_version="0.4.2"
pid0_header="libpid0-${pid0_version}.h"
pid0_header_gz="${pid0_header}.gz"
pid0_url="https://github.com/sa6mwa/libpid0/releases/download/v${pid0_version}/${pid0_header_gz}"
pid0_sha256="907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8"
pid0_download="$archive_cache_root/$pid0_sha256/$pid0_header_gz"
libxml2_version="2.15.3"
lua_version="5.5.0"
manifest_path="$deps_root/manifest.txt"

download_if_missing() {
  url=$1
  out=$2
  expected_sha256=$3
  archive_dir=$(dirname -- "$out")
  archive_name=$(basename -- "$out")
  mkdir -p "$archive_dir" "$archive_lock_root"
  (
    flock -w "${CPKT_DEPENDENCY_LOCK_TIMEOUT:-600}" 9
    if [ -f "$out" ]; then
      actual_sha256=$(sha256sum "$out" | awk '{print $1}')
      if [ "$actual_sha256" = "$expected_sha256" ]; then
        exit 0
      fi
      rm -f "$out"
    fi
    tmp="$archive_dir/.${archive_name}.$$"
    rm -f "$tmp"
    curl -L --fail --retry 3 --output "$tmp" "$url"
    actual_sha256=$(sha256sum "$tmp" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
      rm -f "$tmp"
      echo "checksum mismatch for $archive_name" >&2
      echo "expected $expected_sha256" >&2
      echo "actual   $actual_sha256" >&2
      exit 1
    fi
    mv "$tmp" "$out"
  ) 9>"$archive_lock_root/$expected_sha256.lock"
}

download_if_missing "$system_url" "$system_download" "$system_sha256"
download_if_missing "$lockdc_url" "$lockdc_download" "$lockdc_sha256"
download_if_missing "$lockdc_lua_url" "$lockdc_lua_download" "$lockdc_lua_sha256"
download_if_missing "$lonejson_url" "$lonejson_download" "$lonejson_sha256"
download_if_missing "$lonejson_lua_url" "$lonejson_lua_download" "$lonejson_lua_sha256"
download_if_missing "$pslog_url" "$pslog_download" "$pslog_sha256"
download_if_missing "$pslog_lua_url" "$pslog_lua_download" "$pslog_lua_sha256"
download_if_missing "$cai_url" "$cai_download" "$cai_sha256"
download_if_missing "$cai_lua_url" "$cai_lua_download" "$cai_lua_sha256"
download_if_missing "$lql_url" "$lql_download" "$lql_sha256"
download_if_missing "$lql_lua_url" "$lql_lua_download" "$lql_lua_sha256"
download_if_missing "$mdf_url" "$mdf_download" "$mdf_sha256"
download_if_missing "$mdf_lua_url" "$mdf_lua_download" "$mdf_lua_sha256"
download_if_missing "$softline_url" "$softline_download" "$softline_sha256"
download_if_missing "$softline_lua_url" "$softline_lua_download" "$softline_lua_sha256"
case "$target_cmake_system_name" in
  Linux)
    pid0_enabled=1
    download_if_missing "$pid0_url" "$pid0_download" "$pid0_sha256"
    ;;
esac

actual_system_sha256=$(sha256sum "$system_download" | awk '{print $1}')
if [ "$actual_system_sha256" != "$system_sha256" ]; then
  echo "checksum mismatch for $system_archive" >&2
  echo "expected $system_sha256" >&2
  echo "actual   $actual_system_sha256" >&2
  exit 1
fi
actual_sha256=$(sha256sum "$lockdc_download" | awk '{print $1}')
if [ "$actual_sha256" != "$lockdc_sha256" ]; then
  echo "checksum mismatch for $lockdc_archive" >&2
  echo "expected $lockdc_sha256" >&2
  echo "actual   $actual_sha256" >&2
  exit 1
fi
actual_lockdc_lua_sha256=$(sha256sum "$lockdc_lua_download" | awk '{print $1}')
if [ "$actual_lockdc_lua_sha256" != "$lockdc_lua_sha256" ]; then
  echo "checksum mismatch for $lockdc_lua_archive" >&2
  echo "expected $lockdc_lua_sha256" >&2
  echo "actual   $actual_lockdc_lua_sha256" >&2
  exit 1
fi
actual_lonejson_sha256=$(sha256sum "$lonejson_download" | awk '{print $1}')
if [ "$actual_lonejson_sha256" != "$lonejson_sha256" ]; then
  echo "checksum mismatch for $lonejson_archive" >&2
  echo "expected $lonejson_sha256" >&2
  echo "actual   $actual_lonejson_sha256" >&2
  exit 1
fi
actual_lonejson_lua_sha256=$(sha256sum "$lonejson_lua_download" | awk '{print $1}')
if [ "$actual_lonejson_lua_sha256" != "$lonejson_lua_sha256" ]; then
  echo "checksum mismatch for $lonejson_lua_archive" >&2
  echo "expected $lonejson_lua_sha256" >&2
  echo "actual   $actual_lonejson_lua_sha256" >&2
  exit 1
fi
actual_pslog_sha256=$(sha256sum "$pslog_download" | awk '{print $1}')
if [ "$actual_pslog_sha256" != "$pslog_sha256" ]; then
  echo "checksum mismatch for $pslog_archive" >&2
  echo "expected $pslog_sha256" >&2
  echo "actual   $actual_pslog_sha256" >&2
  exit 1
fi
actual_pslog_lua_sha256=$(sha256sum "$pslog_lua_download" | awk '{print $1}')
if [ "$actual_pslog_lua_sha256" != "$pslog_lua_sha256" ]; then
  echo "checksum mismatch for $pslog_lua_archive" >&2
  echo "expected $pslog_lua_sha256" >&2
  echo "actual   $actual_pslog_lua_sha256" >&2
  exit 1
fi
actual_cai_sha256=$(sha256sum "$cai_download" | awk '{print $1}')
if [ "$actual_cai_sha256" != "$cai_sha256" ]; then
  echo "checksum mismatch for $cai_archive" >&2
  echo "expected $cai_sha256" >&2
  echo "actual   $actual_cai_sha256" >&2
  exit 1
fi
actual_cai_lua_sha256=$(sha256sum "$cai_lua_download" | awk '{print $1}')
if [ "$actual_cai_lua_sha256" != "$cai_lua_sha256" ]; then
  echo "checksum mismatch for $cai_lua_archive" >&2
  echo "expected $cai_lua_sha256" >&2
  echo "actual   $actual_cai_lua_sha256" >&2
  exit 1
fi
actual_lql_sha256=$(sha256sum "$lql_download" | awk '{print $1}')
if [ "$actual_lql_sha256" != "$lql_sha256" ]; then
  echo "checksum mismatch for $lql_archive" >&2
  echo "expected $lql_sha256" >&2
  echo "actual   $actual_lql_sha256" >&2
  exit 1
fi
actual_lql_lua_sha256=$(sha256sum "$lql_lua_download" | awk '{print $1}')
if [ "$actual_lql_lua_sha256" != "$lql_lua_sha256" ]; then
  echo "checksum mismatch for $lql_lua_archive" >&2
  echo "expected $lql_lua_sha256" >&2
  echo "actual   $actual_lql_lua_sha256" >&2
  exit 1
fi
actual_mdf_sha256=$(sha256sum "$mdf_download" | awk '{print $1}')
if [ "$actual_mdf_sha256" != "$mdf_sha256" ]; then
  echo "checksum mismatch for $mdf_archive" >&2
  echo "expected $mdf_sha256" >&2
  echo "actual   $actual_mdf_sha256" >&2
  exit 1
fi
actual_mdf_lua_sha256=$(sha256sum "$mdf_lua_download" | awk '{print $1}')
if [ "$actual_mdf_lua_sha256" != "$mdf_lua_sha256" ]; then
  echo "checksum mismatch for $mdf_lua_archive" >&2
  echo "expected $mdf_lua_sha256" >&2
  echo "actual   $actual_mdf_lua_sha256" >&2
  exit 1
fi
actual_softline_sha256=$(sha256sum "$softline_download" | awk '{print $1}')
if [ "$actual_softline_sha256" != "$softline_sha256" ]; then
  echo "checksum mismatch for $softline_archive" >&2
  echo "expected $softline_sha256" >&2
  echo "actual   $actual_softline_sha256" >&2
  exit 1
fi
actual_softline_lua_sha256=$(sha256sum "$softline_lua_download" | awk '{print $1}')
if [ "$actual_softline_lua_sha256" != "$softline_lua_sha256" ]; then
  echo "checksum mismatch for $softline_lua_archive" >&2
  echo "expected $softline_lua_sha256" >&2
  echo "actual   $actual_softline_lua_sha256" >&2
  exit 1
fi
if [ "$pid0_enabled" -eq 1 ]; then
  actual_pid0_sha256=$(sha256sum "$pid0_download" | awk '{print $1}')
  if [ "$actual_pid0_sha256" != "$pid0_sha256" ]; then
    echo "checksum mismatch for $pid0_header_gz" >&2
    echo "expected $pid0_sha256" >&2
    echo "actual   $actual_pid0_sha256" >&2
    exit 1
  fi
fi
rm -rf "$deps_root/include" "$deps_root/lib" "$deps_root/share"
mkdir -p "$deps_root"
tar -xzf "$system_download" -C "$deps_root" --strip-components 1
tar -xzf "$lockdc_download" -C "$deps_root" --strip-components 1
mkdir -p "$lockdc_lua_source_dir"
tar -xzf "$lockdc_lua_download" -C "$lockdc_lua_source_dir" --strip-components 1
tar -xzf "$lonejson_download" -C "$deps_root" --strip-components 1
mkdir -p "$lonejson_source_dir"
tar -xzf "$lonejson_lua_download" -C "$lonejson_source_dir" --strip-components 1
case "$target_cmake_system_name" in
  Linux)
    if [ -e "$deps_root/lib/liblonejson.so.$lonejson_version" ] && [ ! -e "$deps_root/lib/liblonejson.so.25" ]; then
      ln -s "liblonejson.so.$lonejson_version" "$deps_root/lib/liblonejson.so.25"
    fi
    ;;
esac
tar -xzf "$pslog_download" -C "$deps_root" --strip-components 1
mkdir -p "$pslog_lua_source_dir"
tar -xzf "$pslog_lua_download" -C "$pslog_lua_source_dir" --strip-components 1
tar -xzf "$cai_download" -C "$deps_root" --strip-components 1
mkdir -p "$cai_lua_source_dir"
tar -xzf "$cai_lua_download" -C "$cai_lua_source_dir" --strip-components 1
tar -xzf "$lql_download" -C "$deps_root" --strip-components 1
mkdir -p "$lql_lua_source_dir"
tar -xzf "$lql_lua_download" -C "$lql_lua_source_dir" --strip-components 1
tar -xzf "$mdf_download" -C "$deps_root" --strip-components 1
mkdir -p "$mdf_lua_source_dir"
tar -xzf "$mdf_lua_download" -C "$mdf_lua_source_dir" --strip-components 1
tar -xzf "$softline_download" -C "$deps_root" --strip-components 1
mkdir -p "$softline_lua_source_dir"
tar -xzf "$softline_lua_download" -C "$softline_lua_source_dir" --strip-components 1
if [ "$pid0_enabled" -eq 1 ]; then
  gzip -dc "$pid0_download" > "$deps_root/include/$pid0_header"
fi

cat > "$manifest_path" <<EOF
preset=$preset
target_id=$target_id
system_archive=$system_archive
system_version=$system_version
system_sha256=$system_sha256
liblockdc_archive=$lockdc_archive
liblockdc_version=$lockdc_version
liblockdc_sha256=$lockdc_sha256
lockdc_lua_archive=$lockdc_lua_archive
lockdc_lua_payload=$lockdc_lua_payload
lockdc_lua_sha256=$lockdc_lua_sha256
lonejson_archive=$lonejson_archive
lonejson_version=$lonejson_version
lonejson_sha256=$lonejson_sha256
lonejson_lua_archive=$lonejson_lua_archive
lonejson_lua_payload=$lonejson_lua_payload
lonejson_lua_sha256=$lonejson_lua_sha256
pslog_archive=$pslog_archive
pslog_version=$pslog_version
pslog_sha256=$pslog_sha256
pslog_source=libpslog-release
pslog_lua_archive=$pslog_lua_archive
pslog_lua_sha256=$pslog_lua_sha256
pslog_lua_source=lua-pslog-source-archive
cai_archive=$cai_archive
cai_version=$cai_version
cai_sha256=$cai_sha256
cai_lua_archive=$cai_lua_archive
cai_lua_payload=$cai_lua_payload
cai_lua_sha256=$cai_lua_sha256
cai_source=cai-release
cai_lua_source=cai-lua-source-archive
liblql_archive=$lql_archive
liblql_version=$lql_version
liblql_sha256=$lql_sha256
liblql_source=liblql-release
liblql_lua_archive=$lql_lua_archive
liblql_lua_sha256=$lql_lua_sha256
liblql_lua_source=liblql-lua-source-archive
libmdf_archive=$mdf_archive
libmdf_version=$mdf_version
libmdf_sha256=$mdf_sha256
libmdf_lua_archive=$mdf_lua_archive
libmdf_lua_sha256=$mdf_lua_sha256
libmdf_source=libmdf-release
libmdf_lua_source=libmdf-lua-source-archive
softline_archive=$softline_archive
softline_version=$softline_version
softline_sha256=$softline_sha256
softline_lua_archive=$softline_lua_archive
softline_lua_payload=$softline_lua_payload
softline_lua_sha256=$softline_lua_sha256
softline_source=softline-release
softline_lua_source=softline-lua-source-archive
lockdc_lua_source=lockdc-lua-source-archive
lonejson_lua_source=lonejson-lua-source-archive
curl_source=c.pkt.systems
openssl_source=c.pkt.systems
libssh2_source=c.pkt.systems
nghttp2_source=c.pkt.systems
zlib_source=c.pkt.systems
libpid0_enabled=$pid0_enabled
EOF
if [ "$pid0_enabled" -eq 1 ]; then
  cat >> "$manifest_path" <<EOF
libpid0_version=$pid0_version
libpid0_header=$pid0_header
libpid0_sha256=$pid0_sha256
EOF
fi
cat >> "$manifest_path" <<EOF
lua_version=$lua_version
lua_source=c.pkt.systems
lua_linkage=static+shared
lua_runtime_facade=cpkt-lua-runtime
lua_runtime_abi_version=0
audio_abi_version=0
sus_abi_version=0
opcua_abi_version=0
miniaudio_version=0.11.25
whisper_version=v1.9.1
open62541_version=1.5.4
libxml2_version=$libxml2_version
libxml2_source=c.pkt.systems
libxml2_iconv=enabled
libxml2_zlib=enabled
libxml2_catalog=default
libxml2_linkage=static+shared
EOF
