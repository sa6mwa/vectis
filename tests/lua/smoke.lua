local vectis = require("vectis")

assert(type(vectis) == "table")
assert(vectis.version == "0.0.0")
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")
