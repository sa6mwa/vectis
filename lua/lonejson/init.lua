-- Keep the installed upstream facade authoritative; add only Vectis shortcuts.
local M = require("lonejson.upstream")
function M.schema(name, fields) return M.core.new():schema(name, fields) end
M.array_rewrite_string = M.core.array_rewrite_string
M.array_rewrite_path = M.core.array_rewrite_path
return M
