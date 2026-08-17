local dsv = require("vectis.dsv")
local lonejson = require("lonejson")
local xml = require("vectis.xml")

local row_schema = lonejson.schema("example-row", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("count", lonejson.i64({required = true})),
  lonejson.field("active", lonejson.boolean({required = true})),
})

local rows = assert(dsv.parse({
  schema = row_schema,
  data = "id,count,active\nalpha,2,true\n\"beta,quoted\",3,false\n",
}))
assert(#rows == 2)
assert(rows[1].id == "alpha")
assert(rows[1].count == 2)
assert(rows[2].id == "beta,quoted")
assert(rows[2].active == false)

local seen = {}
assert(dsv.each({
  schema = row_schema,
  data = "# export\ngamma,5,true\ndelta,8,false\n",
  headerless = true,
  columns = {"id", "count", "active"},
  comment_prefix = "#",
  on_row = function(row_number, row)
    seen[#seen + 1] = row_number .. ":" .. row.id .. ":" .. tostring(row.count)
  end,
}) == true)
assert(seen[1] == "1:gamma:5")
assert(seen[2] == "2:delta:8")

local tsv = assert(dsv.to_string({
  schema = row_schema,
  format = "tsv",
  rows = {
    {id = "epsilon", count = 13, active = true},
  },
}))
assert(tsv == "id\tcount\tactive\nepsilon\t13\ttrue\n")

local invoice_schema = lonejson.schema("example-invoice", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("amount", lonejson.object({
    required = true,
    fields = {
      lonejson.field("currency", lonejson.string({required = true})),
      lonejson.field("text", lonejson.f64({required = true})),
    },
  })),
  lonejson.field("line", lonejson.object_array({
    fields = {
      lonejson.field("sku", lonejson.string({required = true})),
      lonejson.field("quantity", lonejson.i64({required = true})),
    },
  })),
  lonejson.field("tag", lonejson.string_array()),
  lonejson.field("active", lonejson.boolean({required = true})),
})

local invoice_xml = table.concat({
  "<invoice id=\"inv-1\">",
  "<amount currency=\"EUR\">12.50</amount>",
  "<line><sku>A-1</sku><quantity>2</quantity></line>",
  "<line><sku>B-2</sku><quantity>5</quantity></line>",
  "<tag>paid</tag><tag>priority</tag>",
  "<active>true</active>",
  "</invoice>",
})
local invoice = assert(xml.parse({
  schema = invoice_schema,
  root_element = "invoice",
  xml = invoice_xml,
  trim_text = true,
}))
assert(invoice.id == "inv-1")
assert(invoice.amount.currency == "EUR")
assert(invoice.amount.text == 12.5)
assert(invoice.line[2].sku == "B-2")
assert(invoice.line[2].quantity == 5)
assert(invoice.tag[2] == "priority")
assert(invoice.active == true)

local serialized_invoice = assert(xml.serialize({
  schema = invoice_schema,
  root_element = "invoice",
  value = {
    id = "inv-&-2",
    amount = {currency = "SEK", text = 42.25},
    line = {
      {sku = "S-1", quantity = 1},
      {sku = "S-2", quantity = 4},
    },
    tag = {"generated", "xml"},
    active = false,
  },
}))
assert(serialized_invoice:find("<id>inv-&amp;-2</id>", 1, true))
assert(serialized_invoice:find("<active>false</active>", 1, true))
local serialized_roundtrip = assert(xml.parse({
  schema = invoice_schema,
  root_element = "invoice",
  xml = serialized_invoice,
  trim_text = true,
}))
assert(serialized_roundtrip.id == "inv-&-2")
assert(serialized_roundtrip.amount.text == 42.25)
assert(serialized_roundtrip.line[2].quantity == 4)
assert(serialized_roundtrip.tag[1] == "generated")
assert(serialized_roundtrip.active == false)

local attr_schema = lonejson.schema("example-item", {
  lonejson.field("@id", lonejson.string({required = true})),
  lonejson.field("text", lonejson.string({required = true})),
})
local attr_xml = assert(xml.serialize({
  schema = attr_schema,
  root_element = "item",
  attribute_prefix = "@",
  value = {["@id"] = "item&1", text = "hello <xml>"},
}))
assert(attr_xml == "<item id=\"item&amp;1\">hello &lt;xml&gt;</item>")
local attr_roundtrip = assert(xml.parse({
  schema = attr_schema,
  root_element = "item",
  attribute_prefix = "@",
  xml = attr_xml,
}))
assert(attr_roundtrip["@id"] == "item&1")
assert(attr_roundtrip.text == "hello <xml>")

print("lua data formats example ok")
