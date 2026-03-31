# Kore Vendoring

`vendor/kore/upstream/` is a local checkout of upstream Kore.

Local changes should be expressed as patch files in `vendor/kore/patches/`
and listed in `vendor/kore/patches/series` in apply order.

Pinned upstream revision currently validated by this repo:

```text
ba9bc3b56e24c3652f4cd808463ad45f78579ba8
```

Use:

```sh
make vendor-kore
make vendor-kore-apply
make build-kore
make verify-kore-patches
make vendor-kore-upgrade
```

Current patch goals:

- link Kore against the bundled dependency root shipped by `liblockdc`
- add a `pslog` logging backend for Kore
- add `lonejson` request helpers and use `lonejson` for ACME parsing
- keep Kore JSON-RPC out of the `vectis` surface
