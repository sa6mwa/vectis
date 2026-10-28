local v, lc = require("vectis"), require("lockdc")
local root = assert(os.getenv("VECTIS_WORKFLOW_TEST_ROOT"), "set VECTIS_WORKFLOW_TEST_ROOT to a disposable Pouch directory")
local key = assert(lc.pouch_crypto_generate_key())
local app = assert(v.app.new({app_name="workflow-example", tls={mode="disabled"}}))
local workflow = app:workflow({namespace="example", client={
  endpoints={"pouch://"..root}, pouch={single_writer=false, crypto_key=key}, logger_disabled=true,
}})
local delivered = 0
workflow:outbox({kind="print", handler=function(job)
  local _, main = coroutine.running(); assert(main)
  assert(job:write_payload({write=function(chunk) io.stdout:write(chunk); return true end}))
  delivered = delivered + 1
  return job:complete({delivery_reference="stdout"})
end})
assert(app:start())
local result = assert(workflow:transaction(function(tx)
  return tx:append_outbox({operation_id="example", effect_id="print", effect_key="example-print",
    kind="print", destination="stdout", content_type="text/plain",
    payload_digest=require("openssl").sha256_materialized("hello workflow\n")}, "hello workflow\n")
end))
assert(#result.outbox_receipts == 1)
assert(app:pump(0))
assert(delivered == 1 and workflow:stats().received_keys == 1)
assert(app:stop())
app:close()
print("supervisor workflow example ok")
