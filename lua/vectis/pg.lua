-- Explicitly materialized application SQL on the exact managed XA branch.
local core = require('vectis.pg.core')
return {from_borrow=core.from_borrow, NULL=core.NULL,
  binary=function(value, oid) return {value=value, oid=oid, binary=true} end}
