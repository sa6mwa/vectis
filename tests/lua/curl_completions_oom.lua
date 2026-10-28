local curl=require('vectis.curl.handles.core')
local multi=assert(curl.multi())
local url='file://'..CURL_OOM_SOURCE
local ids={}
local buffered=assert(multi:add({url=url,protocols='file'}));ids[buffered]=true
local file=assert(multi:add({url=url,protocols='file',download_path=CURL_OOM_OUTPUT}));ids[file]=true
curl_test_finalize_children(multi) -- simulate child-first finalization before use
while assert(multi:step())~=0 do assert(multi:poll(0)) end
curl_test_fail_close()
return function()return multi:completions()end,function(succeeded,result)
 if not succeeded then result=assert(multi:completions()) end
 assert(#result==2)
 for _,completion in ipairs(result) do
  assert(ids[completion.id]);ids[completion.id]=nil
  if completion.id==buffered then assert(completion.ok and #completion.body>0)
  else assert(not completion.ok and completion.id==file and completion.code==23 and completion.error=='failed to close curl download file') end
 end
 assert(next(ids)==nil and #multi:completions()==0 and multi:close())
end
