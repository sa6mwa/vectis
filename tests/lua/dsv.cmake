set(script "${WORK_DIR}/dsv-smoke.lua")
set(csv_file "${WORK_DIR}/dsv-input.csv")

file(WRITE "${csv_file}" "id,count,active\nalpha,2,true\n\"beta,quoted\",3,false\n")
file(WRITE "${script}" [[
local vectis = require("vectis")
local dsv = require("vectis.dsv")
local lonejson = require("lonejson")

local schema = lonejson.schema("dsv-row", {
  lonejson.field("id", lonejson.string({required = true, fixed_capacity = 64})),
  lonejson.field("count", lonejson.i64({required = true})),
  lonejson.field("active", lonejson.boolean({required = true})),
})

local csv_path = assert(arg[1])
local parsed = assert(dsv.parse({
  schema = schema,
  path = csv_path,
}))
assert(#parsed == 2)
assert(parsed[1].id == "alpha")
assert(parsed[1].count == 2, type(parsed[1].count) .. ":" .. tostring(parsed[1].count))
assert(parsed[2].id == "beta,quoted")
assert(parsed[2].active == false)

local raw = assert(dsv.parse({
  data = "a,b\none,2\n",
}))
assert(raw[1].a == "one")
assert(raw[1].b == "2")

local rows = {}
assert(dsv.each({
  schema = schema,
  data = "# export\nzeta,31,true\neta,37,false\n",
  headerless = true,
  columns = {"id", "count", "active"},
  comment_prefix = "#",
  on_row = function(row_number, row)
    rows[#rows + 1] = {row_number = row_number, row = row}
  end,
}) == true)
assert(#rows == 2)
assert(rows[1].row_number == 1)
assert(rows[1].row.id == "zeta")
assert(rows[2].row.count == 37)

local piped = assert(dsv.parse({
  schema = schema,
  data = "theta|41|true\n",
  delimiter = "|",
  headerless = true,
  columns = {"id", "count", "active"},
}))
assert(piped[1].id == "theta")
assert(piped[1].count == 41)

local spilled = assert(dsv.parse_spill({
  schema = schema,
  data = "iota,43,true\n",
  headerless = true,
  columns = {"id", "count", "active"},
  memory_limit_bytes = 8,
  prefix = "vectis-lua-dsv",
}))
assert(spilled.spooled_to_disk == true)
assert(type(spilled.path) == "string")
local spilled_file = assert(io.open(spilled.path, "rb"))
local spilled_json = spilled_file:read("*a")
spilled_file:close()
os.remove(spilled.path)
assert(spilled_json:find('"id":"iota"', 1, true))

local serialized = assert(dsv.to_string({
  schema = schema,
  rows = {
    {id = "alpha,quoted", count = 2, active = true},
    {id = " #comment", count = 3, active = false},
  },
  comment_prefix = "#",
}))
assert(serialized == 'id,count,active\n"alpha,quoted",2,true\n" #comment",3,false\n')

local headerless_tsv = assert(dsv.to_string({
  schema = schema,
  rows = {
    {id = "omega", count = 5, active = true},
  },
  format = "tsv",
  header = false,
  columns = {"active", "id"},
}))
assert(headerless_tsv == "true\tomega\n")

local bad, err = dsv.parse({
  schema = schema,
  data = "id,count,active\nbad,1\n",
})
assert(bad == nil)
assert(type(err) == "table")
assert(err.status == vectis.ERR_INVALID)
assert(err.source == "vectis")
assert(err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(err.message:find("row width", 1, true))

local stopped, stop_err = dsv.each({
  data = "id,count\none,1\ntwo,2\n",
  on_row = function()
    return false
  end,
})
assert(stopped == nil)
assert(type(stop_err) == "table")
assert(stop_err.status == vectis.ERR_STATE)
assert(stop_err.status_string == vectis.status_string(vectis.ERR_STATE))
assert(stop_err.source == "vectis")
assert(stop_err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(stop_err.message == "DSV row callback stopped")

local dynamic_schema = lonejson.schema("dynamic-dsv-row", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("count", lonejson.i64({required = true})),
})
local dynamic_rows = assert(dsv.parse({
  schema = dynamic_schema,
  data = "id,count\nalpha,2\n",
}))
assert(#dynamic_rows == 1)
assert(dynamic_rows[1].id == "alpha")
assert(dynamic_rows[1].count == 2)

local dynamic_seen = {}
assert(dsv.each({
  schema = dynamic_schema,
  data = "id,count\nbravo,3\ncharlie,4\n",
  on_row = function(row_number, row)
    dynamic_seen[#dynamic_seen + 1] = row_number .. ":" .. row.id .. ":" .. tostring(row.count)
  end,
}) == true)
assert(#dynamic_seen == 2)
assert(dynamic_seen[1] == "1:bravo:3")
assert(dynamic_seen[2] == "2:charlie:4")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${csv_file}"
                RESULT_VARIABLE dsv_result
                OUTPUT_VARIABLE dsv_stdout
                ERROR_VARIABLE dsv_stderr)
if(NOT dsv_result EQUAL 0)
  message(FATAL_ERROR "vectis DSV Lua smoke failed: ${dsv_stdout}${dsv_stderr}")
endif()
