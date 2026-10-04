# Lua XML Facade

Vectis preloads `vectis.xml` in the embedded Lua runtime. The facade exposes
the libxml2-backed C SDK XML-to-LoneJSON parser and the C SDK LoneJSON-to-XML
serializer to Lua scripts.

## Entry Points

- `vectis.xml.parse(opts)` parses XML into a Lua table.
- `vectis.xml.parse_record(opts)` parses XML into a Lua-owned LoneJSON record.
- `vectis.xml.serialize(opts)` serializes a Lua table or Lua-owned LoneJSON
  record into a materialized XML string.
- `vectis.xml.write(opts, sink)` writes native XML chunks directly to a sink
  function or `{write=function(chunk) ... end}` table and returns `true, bytes`.

`parse()` is intentionally materialized because it converts the final record to
a Lua table. Use `parse_record()` when the schema contains spooled fields or
when the caller wants to preserve LoneJSON record ownership.

`write()` and `serialize()` share the native `vectis_xml_write_lonejson()`
emitter. Only `serialize()` collects output in memory. `write()` calls the sink
synchronously on the caller's Lua state with chunks of at most 4096 bytes;
there is no whole-output buffer or output staging file. The caller owns the sink,
which is never closed by this operation. A sink may return nothing or success;
`false`, `nil, error`, or a thrown error stops emission and returns `nil, err`
with a structured message. Nested independent writes are permitted.

Callback writes accept `value`/`table` input only. They assign it into a private
record before output starts; option strings and schema remain retained through
callbacks and garbage collection. Mutating the original table or options during
a callback cannot alter that record or its configuration. Spooled input fields
are copied through public schema assignment into private input storage, using
the configured spill policy; this is input preparation, not output spooling.
External record userdata is rejected with an actionable error because public
LoneJSON borrowed views have no mutation lease. Use the explicitly buffered
`serialize()` for external records. Assignment misuse raises after cleanup;
sink failures return structured errors after cleanup.

Parsing builds a mapped record, potentially with spooled fields. It is not an
incremental XML DOM API. The C SDK also exposes `vectis_xml_lonejson_to_bytes()`
for owned byte output.

## Options

- `schema`: required LoneJSON schema userdata from `lonejson.schema(...)`.
- `xml` or `data`: XML string input.
- `path`: XML file path input streamed by the C SDK.
- `root_element`: optional required document root name.
- `text_key`: object field used for mixed/object text, default `text`.
- `attribute_prefix`: prefix used to map XML attributes to fields, default
  empty.
- `trim_text`: trims text spans before scalar assignment.
- `skip_unknown`: default true; false rejects unknown XML elements or
  attributes.
- `strict_unknown`: alias for `skip_unknown = false`.
- `max_depth`: nesting limit, default 64.
- `max_text_bytes`: scalar text limit, default 64 MiB.

For serialization, pass either `record` with a Lua-owned LoneJSON record or
`value`/`table` with a Lua table that can be assigned through the schema.
`root_element` defaults to the LoneJSON schema name. Fields whose key equals
`text_key` are written as element text. If `attribute_prefix` is non-empty,
fields whose key starts with that prefix are written as XML attributes with the
prefix stripped. Arrays are written as repeated elements in schema order.

## Example

```lua
local lonejson = require("lonejson")
local xml = require("vectis.xml")

local schema = lonejson.schema("invoice", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("amount", lonejson.object({
    required = true,
    fields = {
      lonejson.field("currency", lonejson.string({required = true})),
      lonejson.field("text", lonejson.f64({required = true})),
    },
  })),
})

local doc, err = xml.parse({
  schema = schema,
  root_element = "invoice",
  xml = "<invoice id=\"inv-1\"><amount currency=\"EUR\">12.50</amount></invoice>",
})
assert(doc, err and err.message)

local out = assert(xml.serialize({
  schema = schema,
  root_element = "invoice",
  value = {
    id = "inv-1",
    amount = {currency = "EUR", text = 12.50},
  },
}))
assert(out:find("<invoice>", 1, true))
```
