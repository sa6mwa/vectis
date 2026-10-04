-- Ordinary Lua 5.5: artifact path first, authoritative dependency path second.
local source, upstream=assert(arg[1]),assert(arg[2])
package.path=source..'/?.lua;'..source..'/?/init.lua;'..upstream..'/?.lua;'..upstream..'/?/init.lua'
package.cpath=''
assert(next(package.preload)==nil, 'ordinary Lua probe requires no preloaded dependency stubs')
local v=require('vectis')
assert(v.supervisor==require('vectis.supervisor') and v.status==require('vectis.status'))
assert(v.libs.curl==nil and v.libs.lonejson==nil and v.lockd==nil and v.workflow==nil)
assert(type(v.cli)=='table' and type(v.kore)=='table' and v.app==nil)
package.loaded.vectis=nil
local function failure_probe(name, value)
  package.preload[name]=function() error(value,0) end
  local ok,err=pcall(require,'vectis'); assert(not ok and err==value)
  package.preload[name]=nil; package.loaded.vectis=nil
end
failure_probe('opcua','string initialization failure')
failure_probe('curl',"initialization: module 'curl.core' not found:\n\tno field package.preload['curl.core']")
local failure={message='artifact loader failure'}
failure_probe('opcua',failure)
package.preload.opcua=function() return require('unknown.opcua.prerequisite') end
local ok,err=pcall(require,'vectis'); assert(not ok and err:find("unknown.opcua.prerequisite",1,true))
package.preload.opcua=nil; package.loaded.vectis=nil
-- Custom searchers may make an otherwise absent root available: availability
-- comes from require, never from an inferred package.path existence check.
table.insert(package.searchers, 1, function(name)
  if name=='opcua' then return function() return {custom=true} end end
  return '\n\tcustom searcher: no module'
end)
assert(require('vectis').libs.opcua.custom)
print('source rock genuine source-only behavior ok')
-- Separate availability/identity fixture; no native behavior is claimed.
package.loaded.vectis=nil; package.loaded.opcua=nil; table.remove(package.searchers,1)
local core={json_null=function() return {} end}
package.preload['lonejson.core']=function() return core end
local json=require('lonejson')
assert(json.core==core and type(json.new)=='function')
assert(package.loaded['lonejson.upstream']==nil, 'source rock must preserve canonical upstream root')
local v=require('vectis')
assert(v.libs.lonejson==json and v.supervisor==require('vectis.supervisor'))
assert(v.libs.opcua==nil and v.workflow==nil) -- legitimate missing native roots
package.loaded.vectis=nil
local failure={message='artifact loader failure'}
package.preload.opcua=function() error(failure,0) end
local ok,err=pcall(require,'vectis'); assert(not ok and err==failure)
print('source rock artifact path behavior ok')
