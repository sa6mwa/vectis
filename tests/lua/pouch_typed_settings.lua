local v, lc = require("vectis"), require("lockdc")
local root = assert(os.getenv("VECTIS_UPGRADE_TEST_ROOT"))
local key = assert(lc.pouch_crypto_generate_key())
local wrong_key = assert(lc.pouch_crypto_generate_key())
local path = root .. "/typed root ?#%"
local endpoint = assert(v.lockd.pouch_endpoint_build(path, {
  {name="single_writer", value="true"}, {name="crypto_key", value=wrong_key},
  {name="compression", value="none"},
}))
local pouch = {
  single_writer=false, durable_sync=false, fsync_batch_max_ops=0,
  segment_target_bytes=1048576, indexer_flush_docs=0,
  indexer_flush_interval_seconds=0, background_compaction=false,
  disable_compaction_throttling=false, terminal_reclaim_min_bytes=0,
  queue_watch=false, query_engine="scan", query_fallback_engine="scan",
  query_indexing=false, crypto_key=key, crypto_generate_key_file=false,
  compression="zlib",
}
local options = {tls={mode="disabled"}, lockd={endpoints={endpoint}, pouch=pouch}}
local app, err = v.app.new(options)
assert(app, err and err.message)
-- Mutating the original cannot change the captured native root policy.
pouch.single_writer, pouch.crypto_key, pouch.compression = true, wrong_key, "none"
collectgarbage("collect")
assert(app:start())
-- Independent shared opening proves that the native app did not select the
-- endpoint's exclusive-writer flag or the endpoint's wrong key.
local shared, open_error = lc.open({
  endpoints={assert(v.lockd.pouch_endpoint_build(path))},
  pouch={single_writer=false, crypto_key=key, compression="zlib"},
})
assert(shared, open_error and open_error.message)
shared:close()
assert(app:stop())
app:close()

for _, invalid in ipairs({
  {single_writer="false"}, {durable_sync=0}, {fsync_batch_max_ops=-1},
  {segment_target_bytes=1.5}, {unknown=true}, {compression="gzip"},
  {query_engine="indexed"}, {crypto_key=""}, {crypto_key="key\0suffix"},
  {indexer_flush_docs={high=4294967296,low=0}},
  {indexer_flush_docs={high=0,low=0,extra=1}},
  {["single_writer\0suffix"]=false},
}) do
  local ok, result = pcall(v.app.new, {tls={mode="disabled"}, lockd={pouch=invalid}})
  assert(not ok or result == nil, "invalid typed settings accepted")
end
local remote = v.app.new({tls={mode="disabled"}, lockd={
  endpoints={"https://unused.example.test"},pouch={single_writer=false}}})
assert(remote == nil)
-- High-word values must not be narrowed, but this declaration creates no root
-- or background worker and is closed without startup.
local wide = assert(v.app.new({tls={mode="disabled"}, lockd={
  pouch={fsync_batch_max_ops={high=0xffffffff,low=0xffffffff}}}}))
wide:close()
local normalized = v.supervisor.client_config({
  pouch_crypto_key=wrong_key, pouch_crypto_generate_key_file=true,
  pouch={crypto_key=key,crypto_generate_key_file=false},
})
assert(normalized.pouch.crypto_key == key)
assert(normalized.pouch.crypto_generate_key_file == false)
assert(normalized.pouch_crypto_key == nil)
local changed = assert(v.app.new({tls={mode="disabled"}}))
assert(changed:set_pouch_settings({single_writer=false,query_engine="scan"}))
assert(v.supervisor.state(changed).lockd.pouch.query_engine == "scan")
changed:supervisor({name="captured"})
assert(not pcall(changed.set_pouch_settings,changed,{single_writer=true}))
changed:close()

print("upgrade seam behavior ok: typed Pouch settings")
