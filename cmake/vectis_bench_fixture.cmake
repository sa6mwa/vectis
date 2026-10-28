include_guard(GLOBAL)
function(vectis_add_bench_fixture output)
  set(binary "${CMAKE_BINARY_DIR}/bench/vectis_bench_fixture")
  if(NOT TARGET vectis_bench_fixture)
    find_program(VECTIS_GO_EXECUTABLE NAMES go REQUIRED)
    add_custom_command(OUTPUT "${binary}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/build/go-cache" "${CMAKE_BINARY_DIR}/bench/tmp"
      COMMAND "${CMAKE_COMMAND}" -E env CGO_ENABLED=0 GOPROXY=off GOTOOLCHAIN=local GO111MODULE=off GOMAXPROCS=2 GOFLAGS=-p=2
        "TMPDIR=${CMAKE_BINARY_DIR}/bench/tmp" "GOCACHE=${CMAKE_SOURCE_DIR}/build/go-cache"
        "GOPATH=${CMAKE_SOURCE_DIR}/build/go-path" "GOMODCACHE=${CMAKE_SOURCE_DIR}/build/go-modcache"
        "${VECTIS_GO_EXECUTABLE}" build -trimpath -o "${binary}"
        "${CMAKE_SOURCE_DIR}/bench/proxy_http.go" "${CMAKE_SOURCE_DIR}/bench/proxy_bench.go" "${CMAKE_SOURCE_DIR}/bench/metrics_fixtures.go" "${CMAKE_SOURCE_DIR}/bench/upload_fixture.go"
      DEPENDS "${CMAKE_SOURCE_DIR}/bench/proxy_http.go" "${CMAKE_SOURCE_DIR}/bench/proxy_bench.go" "${CMAKE_SOURCE_DIR}/bench/metrics_fixtures.go" "${CMAKE_SOURCE_DIR}/bench/upload_fixture.go"
      VERBATIM)
    add_custom_target(vectis_bench_fixture ALL DEPENDS "${binary}")
  endif()
  set(${output} "${binary}" PARENT_SCOPE)
endfunction()
