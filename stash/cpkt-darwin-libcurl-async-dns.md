# c.pkt.systems Darwin libcurl disables asynchronous DNS

## Owner and impact

The `c.pkt.systems` 0.10.0 `arm64-apple-darwin` bundle ships libcurl 8.22.0
without `CURL_VERSION_ASYNCHDNS`. Vectis's reverse proxy requires asynchronous
DNS so a worker cannot block on an upstream hostname lookup. The proxy rejects
every HTTP and WebSocket transfer at submission, including requests to numeric
loopback addresses, because it checks the capability when initializing its
libcurl multi pools. Native macOS HTTP, SSE, WS, and WSS proxy behavior remains
untested beyond this gate. This is a bundle configuration issue, not evidence
of a libcurl or Kore runtime defect.

## Reproduction and evidence

- Vectis dependency pin: `scripts/deps.sh` selects
  `c.pkt.systems-0.10.0-arm64-apple-darwin.tar.gz`, SHA-256
  `f63f0e6e847108b287726dbd8d2566ee9255d2c744d8cfcf3b192a4ca791de77`.
- Native `macos-26` arm64 run:
  https://github.com/sa6mwa/vectis/actions/runs/36192250879
- The dedicated `vectis_unit_proxy_curl_capabilities` test reports:

  ```text
  libcurl 8.22.0 lacks CURL_VERSION_ASYNCHDNS (features=0x5129020d)
  ```

- Five live proxy tests return immediate `HTTP/1.1 502 Bad Gateway` with
  `proxy upstream unavailable` or `proxy WebSocket setup failed`; local route
  overlap, process supervision, and Darwin thread inspection tests pass.
- Vectis checks this capability in `src/vectis_proxy_curl.c`,
  `vectis_proxy_curl_worker_init()`. It returns
  `proxy requires asynchronous libcurl DNS` when the bit is absent.
- The upstream source on `trunk` explicitly sets
  `-DENABLE_THREADED_RESOLVER=OFF` for Darwin in
  [`cmake/CpktDependencies.cmake`](https://github.com/sa6mwa/c.pkt.systems/blob/trunk/cmake/CpktDependencies.cmake),
  `cpkt_get_curl_platform_cmake_args()`. The bundle result matches that setting.

The focused consumer check is reproducible on a native arm64 macOS host with
the Vectis branch containing this report:

```sh
brew install cmake ninja coreutils gnu-sed util-linux
CC=clang AR=ar RANLIB=ranlib bash scripts/deps.sh deps-arm64-apple-darwin
VECTIS_KORE_UPSTREAM_URL=https://github.com/jorisvink/kore.git \
  bash scripts/vendor-kore.sh apply
otool_path=$(xcrun --find otool)
cmake --preset darwin-debug \
  -DCMAKE_OTOOL:FILEPATH="$otool_path" \
  -DCPKT_OTOOL:FILEPATH="$otool_path" \
  -D_cai_otool:FILEPATH="$otool_path" \
  -DVECTIS_OTOOL:FILEPATH="$otool_path"
cmake --build --preset darwin-debug --target vectis_unit_proxy_curl_capabilities
ctest --preset darwin-debug --output-on-failure \
  -R '^vectis_unit_proxy_curl_capabilities$'
```

The source is also directly visible in Vectis's
[`test_proxy_curl_capabilities.c`](../tests/unit/test_proxy_curl_capabilities.c).

## Requested upstream fix

Enable libcurl's threaded resolver for Darwin in
`cpkt_get_curl_platform_cmake_args()` and rebuild the Darwin bundle. Verify the
resulting *packaged* static and shared libcurl both report
`CURL_VERSION_ASYNCHDNS`. Keep the existing Security framework/trust-store
configuration. If enabling the resolver breaks the cross build, fix that build
issue rather than shipping a synchronous resolver: synchronous hostname lookup
would block Vectis's event-loop worker and violate the proxy's concurrency
contract. Add a native Darwin capability test as a release gate.

The capability bit only proves resolver selection. Also run a native test that
requests a hostname through the libcurl multi socket API while another
connection continues to make progress, then consume the rebuilt archive from
Vectis's macOS workflow. The latter runs the HTTP/SSE/WS/WSS integration suite
and will expose any later kqueue or TLS issue separately.

## Building and testing macOS artifacts with GitHub Actions

The Vectis workflow
[`darwin-kqueue.yml`](../.github/workflows/darwin-kqueue.yml) already builds
and runs arm64 binaries on `macos-26`. The c.pkt.systems repository currently
has an osxcross release preset but no native Darwin configure preset. Add a
native preset or configure without its osxcross toolchain, and register a
`cpkt_curl_capabilities_test` target linked to `cpkt::curl_shared` (and an
equivalent static check). A focused upstream workflow can then use:

```yaml
name: Native Darwin bundle
on:
  push:
  pull_request:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  arm64:
    runs-on: macos-26
    timeout-minutes: 120
    steps:
      - uses: actions/checkout@v4
      - name: Install build tools
        run: brew install cmake ninja coreutils gnu-sed gnu-tar util-linux
      - name: Configure native arm64 build
        run: |
          cmake -S . -B build/darwin-native -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCPKT_TARGET_ARCH=arm64 -DCPKT_TARGET_OS=darwin \
            -DCPKT_TARGET_LIBC= \
            -DCPKT_BUILD_DEPENDENCIES=ON -DCPKT_BUILD_TESTS=ON \
            -DCMAKE_OTOOL:FILEPATH="$(xcrun --find otool)"
      - name: Build and test packaged dependencies
        run: |
          cmake --build build/darwin-native --parallel 2
          ctest --test-dir build/darwin-native --output-on-failure
          cmake --build build/darwin-native --target package-bundle
      - name: Preserve macOS artifacts
        uses: actions/upload-artifact@v4
        with:
          name: cpkt-arm64-apple-darwin
          path: dist/*arm64-apple-darwin*
          if-no-files-found: error
```

Treat this YAML as an upstream starting point, not a tested c.pkt.systems
workflow: native configuration and packaging may reveal additional host-tool
assumptions. The existing `arm64-apple-darwin-release` preset uses an osxcross
toolchain and can continue to validate cross builds; it cannot execute the
resulting arm64 binaries on its Linux host. Publish/upload artifacts only after
the native capability and package tests pass. The upstream unit responsible
for the bundle can use the Actions run logs and uploaded archive to validate
both build output and actual macOS behavior before issuing a replacement
release and updating Vectis's pinned checksum.
