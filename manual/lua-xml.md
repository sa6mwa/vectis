# Lua XML Facade

Vectis preloads `vectis.xml` in the embedded Lua runtime. The facade exposes
the libxml2-backed C SDK XML-to-LoneJSON parser and the C SDK LoneJSON-to-XML
serializer, owned DOM/XPath editing and incremental SAX input to Lua scripts.

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

The mapped parse entry points build a record, potentially with spooled fields.
The independent document and SAX APIs are described below. The C SDK also exposes `vectis_xml_lonejson_to_bytes()`
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

## Owned documents and XPath

`xml.document(bytes, opts)` parses a materialized document. Options are
`max_bytes` (default 16 MiB, at most 2147483647) and `max_xpath_nodes` (4096).
These bound input and copied materialized results, not every native allocation.
The document exposes `root()`, `xpath(expression, namespaces)`,
`serialize_materialized(formatted_boolean)` and `close()`.

XPath namespaces are a prefix-to-URI map, at most 256 entries. Results are Lua
numbers, booleans, strings, or arrays of element views in document order.
Non-element node sets are outside this slice: use `string(...)`, `count(...)`,
or an element's attribute/text accessors. Expressions are at most 1 MiB.

An element view exposes:

- `name()` returns local name, namespace URI and prefix (absent values are nil).
- `text()` materializes descendant text. `attribute(name, namespace_uri)` copies
  an attribute, returning nil for absence and `""` for an empty value.
- `set_text(bytes)` replaces children with literal XML UTF-8 text.
- `set_attribute(name, value, opts)` writes literal text; opts may contain
  `namespace` and `prefix`. Namespaced attributes require a prefix. Conflicting
  or reserved namespace bindings are rejected.
- `append_child(name, opts)` returns a child view. Options are `namespace`,
  `prefix` and literal `text`; an omitted namespace inherits the parent's.
- `remove()` removes an element; removing the document root is rejected.
- `close()` releases the view without removing the element.

Every mutation invalidates all prior views, including its receiver. Reacquire
views through `root()` or `xpath()`. A newly appended child is valid until the
next mutation. Views retain their document owner; explicit document close
invalidates them. Receivers belong to their creating process and thread.
Serialization is explicitly materialized, including the native serializer's
buffer. Runtime failures return `nil, error` with operation, copied diagnostic,
status and dependency code; invalid Lua argument shapes raise errors.

```lua
local xml = require('vectis.xml')
local doc = assert(xml.document('<items><item id="1"/></items>'))
local item = assert(doc:xpath('/items/item'))[1]
assert(item:set_text('literal <value>'))
local bytes = assert(doc:serialize_materialized(true))
assert(bytes:find('literal &lt;value&gt;', 1, true))
assert(doc:close())
```

## Caller-fed SAX input

`xml.sax(callbacks, opts)` returns `feed(bytes)`, `finish()` and `close()`.
Each feed accepts at most 65536 bytes. Events run synchronously during feed or
finish; the parser builds no DOM and imposes no cumulative input-size ceiling.
Native XML token/depth limits still apply. `finish()` checks completeness and
is idempotent after success. Close invokes no callbacks.

Callbacks are copied at construction; any subset may be supplied:

- `start_element(name, attributes, namespace)` receives an array of
  `{name, value, namespace, prefix}` attribute records. Namespace records contain
  `uri`, `prefix`, and a `declarations` array of `{prefix, uri}` records.
- `end_element(name, namespace)` receives the element's `uri` and `prefix`.
- `text(bytes)` receives copied UTF-8 chunks of at most 65536 bytes. Event
  boundaries need not match feed or text-node boundaries.

Callback exceptions and explicit `nil, error` stop and latch the parser;
subsequent operations return the same callback error object. Other returns
(including false) are ignored. Receiver reentry/close during a callback fails
the outer operation even when caught. Callbacks cannot yield. Start events are
limited to 256 combined attributes/namespace declarations; their copied metadata
is bounded by `max_metadata_bytes` (default 16 MiB). No full-value collection is
hidden behind SAX.

Both document and SAX parsing exclude DTD/entity declarations and use
context-specific external resource rejection; predefined entities work. Schema,
XInclude, XSLT, external loaders and non-element view graphs are outside this
useful slice. Reconsider declarations or additional node types when a concrete
application requires them.
