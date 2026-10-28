local core=require('vectis.compression.core')
for _,algorithm in ipairs({'zstd','lz4','xz'}) do
  local encoded={}
  local compressor <close> = assert(core.new(algorithm,'compress',{level=1}))
  assert(compressor:process('hello\0binary',function(chunk) encoded[#encoded+1]=chunk; return true end))
  assert(compressor:finish(function(chunk) encoded[#encoded+1]=chunk; return true end))
  local decoder <close> = assert(core.new(algorithm,'decompress'))
  local decoded={}
  assert(decoder:process(table.concat(encoded),function(chunk) decoded[#decoded+1]=chunk; return true end))
  assert(decoder:finish(function(chunk) decoded[#decoded+1]=chunk; return true end))
  assert(table.concat(decoded)=='hello\0binary')
end
return true
