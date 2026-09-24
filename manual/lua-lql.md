# Lua lql

`require("lql")` is the dependency-native liblql Lua module bundled into the
`vectis` runner. It exposes selector, projection, mutation, file filtering, and
spooled rewrite helpers directly from liblql.

## Entry Points

- `lql.new([opts])` creates a client handle.
- `lql.version()` returns the linked liblql version string.
- `lql.status_string(code)` returns a liblql status string.
- `lql.path_is_regular_file(path)` checks a path using liblql's file helper.

Client methods include:

- `client:version()`
- `client:capabilities()`
- `client:selector_parse(expr)`
- `client:selector_parse_or(expr)`
- `client:selector_parse_json(json)`
- `client:selector_capabilities()`
- `client:projection_parse(expr)`
- `client:mutation_parse(expr)`
- `client:apply_string_spooled(opts)`
- `client:filter_file_spooled(opts)`
- `client:rewrite_file_inline_spooled(opts)`

Selector handles expose `selector:capabilities()` and `selector:is_empty()`.
Projection handles expose `projection:path_count()` and
`projection:path(index)`. Mutation handles expose `mutation:count()`.

## Example

```lua
local lql = require("lql")

local client = assert(lql.new())
local capabilities = client:capabilities()
assert(capabilities.selector_parse == true)
assert(capabilities.apply_string_spooled == true)

local selector = assert(client:selector_parse("$.items[*]"))
assert(selector:is_empty() == false)

local projection = assert(client:projection_parse("$.id,$.status"))
assert(projection:path_count() == 2)
```

The module is intentionally dependency-native. Do not add a Vectis-owned helper
unless a repeated workflow crosses Vectis concepts such as HTTP routes, packed
assets, lockd, or service logging.
