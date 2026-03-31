# vectis

`vectis`, or `veCtis`, is an API-first C framework for building workflow-driven services. It is designed for applications that expose HTTP APIs, coordinate state through [lockd](https://github.com/sa6mwa/lockd), consume queue messages, call out to other systems, and run long-lived business workflows inside one coherent runtime.

The design target is enterprise API workloads where a service must do more than answer isolated requests. A `vectis` application should be able to accept an HTTPS request, validate and process large JSON payloads efficiently, read or update workflow state in lockd, enqueue or consume follow-up work, call out to other systems, and emit structured logs that are immediately usable in containerized or orchestrated environments.

`vectis` is aiming to be a serious API framework for workflow orchestration and stateful service integration in a small, performant package. It is intended to fit comfortably in resource-constrained environments while still giving the application first-class HTTP, workflow, queue, logging, and JSON capabilities. A backend-for-frontend is a valid use-case, but it is not the defining one.

## Components

`vectis` is built by integrating a small set of focused C components:

- Kore provides the high-performance HTTP/TLS server runtime, process model,
  and ACME/TLS foundation that `vectis` builds on.
- `liblockdc` provides the lockd client, giving `vectis` a shared API for state
  storage, leases, queue operations, and workflow orchestration. In this model,
  lockd is not just a key-value store. It is a coordination layer for durable
  workflow state, queue-driven workers, and message-based progression through a
  business process.
- `lonejson` provides the JSON layer. `vectis` is intended
  to handle APIs that may carry very large documents, very large strings, or
  large binary values represented in JSON-adjacent forms. `lonejson` is the
  basis for streaming-oriented, low-allocation parsing and serialization
  strategies instead of forcing every request into an eager in-memory DOM.
- `libpslog` provides structured logging. JSON logs to `stdout` or `stderr` are
  a first-class operational target because `vectis` is meant to run cleanly in
  containers and other environments where logs are ingested by the platform
  rather than managed as ad hoc text files.

The point of `vectis` is not merely to bundle those libraries. The point is to present them as one runtime: one app object, one logger surface, one shared lockd client configuration, one HTTP server boundary, and eventually one consistent programming model for handlers and consumers that participate in the same workflow state.

`vectis` itself is kept to a C89 public and internal surface for portability and integration. That constraint applies to the `vectis` API and implementation, and the `vectis` library is built with `-std=c89`. Bundled or vendored dependencies such as Kore, OpenSSL, and libcurl may use their own language standards internally.

## Repository Structure

This repository is intentionally organized as a CMake-first project with a thin `Makefile` entrypoint for common workflows. The structure is meant to support real release engineering rather than just local development:

- CMake presets define the standard build matrix for debug, sanitizers,
  coverage, fuzzing, and Linux release targets.
- Dependency provisioning downloads release artifacts into `.cache/` so a clean
  checkout can build from scratch in a repeatable way.
- Unit tests and fuzz targets are part of the project layout from the start.
- Kore is vendored as an upstream checkout plus an explicit downstream patch
  stack so upstream upgrades remain understandable and mechanically verifiable.

The intent is that `make clean` returns the repository to a true local clean slate by removing generated build output, downloaded dependency bundles, and the generated Kore checkout.

## Current State

The current implementation provides:

- The initial `vectis` runtime/config API and method-table surface.
- Thread-safe app object lifecycle and route registry management.
- `lonejson`-backed JSON validation helpers.
- `pslog`-backed owned or borrowed logger handling.
- Automated dependency provisioning for `liblockdc`, `pslog`, and `lonejson`.
- A Kore upstream checkout plus tracked patch-series workflow.
- A patched Kore build path that links against the bundled `liblockdc` OpenSSL
  and libcurl toolchain.
- A C89 portability contract for the `vectis` public API surface and internal
  implementation.

The actual Kore runtime integration, lockd client bootstrap, consumer runtime,
and TLS server execution remain tracked in [TODO.md](TODO.md).

## Build

Typical local development flow:

```sh
make build
make test
```

The default debug flow provisions dependencies into `.cache/deps/host-debug`.

## Kore Workflow

```sh
make vendor-kore
make vendor-kore-apply
make build-kore
make verify-kore-patches
make vendor-kore-upgrade
```

`vendor/kore/upstream/` is a local checkout of `https://git.kore.io/kore.git`.
Patch files are stored in `vendor/kore/patches/` and ordered by
`vendor/kore/patches/series`.

Current Kore policy in `vectis`:

- `vectis` does not expose Kore JSON-RPC and does not enable Kore's `JSONRPC`
  build feature.
- Kore ACME JSON parsing is patched to use `lonejson`.
- Kore logging is being moved behind a `pslog` backend so HTTP/runtime logs can
  converge on one structured logger.
