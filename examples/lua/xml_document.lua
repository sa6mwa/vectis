local xml = require('vectis.xml')
local doc = assert(xml.document('<invoice><amount currency="EUR">12.50</amount></invoice>'))
local amount = assert(doc:xpath('/invoice/amount'))[1]
print(amount:attribute('currency'), amount:text())
assert(amount:set_text('13.00'))
local output = assert(doc:serialize_materialized(true))
assert(doc:close())
local total = 0
local parser = assert(xml.sax({text=function(bytes) total=total+#bytes end}))
for offset=1,#output,32 do assert(parser:feed(output:sub(offset,offset+31))) end
assert(parser:finish() and parser:close())
print('Text bytes delivered:', total)
