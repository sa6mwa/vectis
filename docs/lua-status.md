# Vectis Lua Status

`vectis.status` is a pure-Lua mirror of the public Vectis status and error
source constants. It is preloaded before other Vectis-owned Lua helper modules
so those helpers can build structured errors without recursively loading the
top-level C-owned `vectis` module.

It exposes:

- status constants: `OK`, `ERR_INVALID`, `ERR_NOMEM`, `ERR_STATE`,
  `ERR_CONFLICT`, `ERR_NOT_IMPLEMENTED`, and `ERR_TIMEOUT`
- error source constants: `ERROR_SOURCE_NONE`, `ERROR_SOURCE_VECTIS`,
  `ERROR_SOURCE_KORE`, `ERROR_SOURCE_LOCKDC`, `ERROR_SOURCE_LONEJSON`,
  `ERROR_SOURCE_PSLOG`, `ERROR_SOURCE_CURL`, `ERROR_SOURCE_OPENSSL`, and
  `ERROR_SOURCE_LIBSSH2`
- `status_string(status)`
- `error_source_string(source)`
- `decorate_error(err, defaults)`
- `error(opts)`

`decorate_error` preserves existing protocol-specific fields such as
`kind`, `code`, `code_name`, `attempts`, and `body`, while adding
`status`, `status_string`, `source`, `source_code`, and optional
`dependency_code`, `http_status`, and `detail`.
