-- Explicitly materialized application SQL on the exact managed XA branch.
local core = require('vectis.pg.core')
local connections = require('vectis.pg.connection.core')
return {from_borrow=core.from_borrow, NULL=core.NULL,
  connect=connections.connect,connect_start=connections.connect_start,
  binary=function(value, oid) return {value=value, oid=oid, binary=true} end}
