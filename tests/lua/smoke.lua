local auth = require("vectis.auth")
local auth_core = require("vectis.auth.core")
local cert = require("vectis.cert")
local embedded = require("vectis.embedded")
local mailbox = require("vectis.mailbox")
local app_module = require("vectis.app")
local obsolete_server_module_ok = pcall(require, "vectis.server")
assert(obsolete_server_module_ok == false)
local ssh = require("vectis.ssh")
local vectis = require("vectis")
local lockdc = require("lockdc")
local lonejson = require("lonejson")
local cai = require("cai")
local lql = require("lql")
local pslog = require("pslog")
local libmdf = require("libmdf")
local softline = require("softline")
local curl = require("curl")
local openssl = require("openssl")
local zlib = require("zlib")
local opcua = require("opcua")
local audio = require("audio")
local sus = require("sus")
local status = require("vectis.status")
local log = require("vectis.log")
local rest = require("vectis.rest")
local terminal = require("vectis.terminal")
local webdav = require("vectis.webdav")
local mqtt = require("vectis.mqtt")
local http = require("vectis.http")
local vcai = require("vectis.cai")
local lockd = require("vectis.lockd")
local dsv = require("vectis.dsv")
local xml = require("vectis.xml")
assert(require("vectis.kore"))

local function base64_encode(input)
  local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  local index = 1
  for i = 1, #input, 3 do
    local a = input:byte(i)
    local b = input:byte(i + 1)
    local c = input:byte(i + 2)
    local triple = a * 65536 + (b or 0) * 256 + (c or 0)
    local first = math.floor(triple / 262144) % 64 + 1
    local second = math.floor(triple / 4096) % 64 + 1
    local third = math.floor(triple / 64) % 64 + 1
    out[index] = alphabet:sub(first, first)
    out[index + 1] = alphabet:sub(second, second)
    out[index + 2] = b and alphabet:sub(third, third) or "="
    out[index + 3] = c and alphabet:sub(triple % 64 + 1, triple % 64 + 1) or "="
    index = index + 4
  end
  return table.concat(out)
end

assert(type(vectis) == "table")
assert(vectis.version == (os.getenv("VECTIS_EXPECTED_VERSION") or "0.0.0"))
assert(type(vectis.libs) == "table")
assert(vectis.libs.lockdc == lockdc)
assert(vectis.libs.lonejson == lonejson)
assert(vectis.libs.cai == cai)
assert(vectis.libs.lql == lql)
assert(vectis.libs.pslog == pslog)
assert(vectis.libs.libmdf == libmdf)
assert(vectis.libs.softline == softline)
assert(vectis.libs.curl == curl)
assert(vectis.libs.openssl == openssl)
assert(vectis.libs.zlib == zlib)
assert(vectis.libs.opcua == opcua)
assert(vectis.libs.audio == audio)
assert(vectis.libs.sus == sus)
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(vectis.status_string(vectis.ERR_NOMEM) == "nomem")
assert(vectis.status_string(vectis.ERR_STATE) == "state")
assert(vectis.status_string(vectis.ERR_CONFLICT) == "conflict")
assert(vectis.status_string(vectis.ERR_NOT_IMPLEMENTED) == "not_implemented")
assert(vectis.status_string(vectis.ERR_TIMEOUT) == "timeout")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_VECTIS) == "vectis")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_LIBSSH2) == "libssh2")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_CPKT) == "cpkt")
assert(vectis.error_source_string(vectis.ERROR_SOURCE_CAI) == "cai")
assert(type(vectis.sleep) == "function")
assert(type(vectis.sleep_ms) == "function")
assert(type(vectis.mkdir_p) == "function")
assert(vectis.sleep_ms(0) == true)
assert(vectis.sleep(0) == true)
do
  local mkdir_root = os.tmpname()
  os.remove(mkdir_root)
  assert(vectis.mkdir_p(mkdir_root .. "/nested/path") == true)
  local mkdir_probe =
      assert(io.open(mkdir_root .. "/nested/path/probe.txt", "wb"))
  mkdir_probe:write("ok\n")
  mkdir_probe:close()
  os.remove(mkdir_root .. "/nested/path/probe.txt")
  os.remove(mkdir_root .. "/nested/path")
  os.remove(mkdir_root .. "/nested")
  os.remove(mkdir_root)
end
assert(status.status_string(status.ERR_INVALID) == "invalid")
assert(status.error_source_string(status.ERROR_SOURCE_CURL) == "curl")
assert(status.error_source_string(status.ERROR_SOURCE_CPKT) == "cpkt")
assert(status.error_source_string(status.ERROR_SOURCE_CAI) == "cai")
assert(vectis.status == status)
assert(vectis.auth == auth)
assert(auth.core == auth_core)
assert(type(auth.workflow) == "function")
assert(vectis.mailbox == mailbox)
assert(vectis.curl_worker == require("vectis.curl_worker"))
assert(vectis.curl_worker.HTTP_KIND == "vectis.curl.http")
assert(vectis.curl_worker.HTTP_REPLY_KIND == "vectis.curl.http.reply")
assert(type(vectis.curl_worker.http_request) == "function")
assert(type(vectis.curl_worker.decode_http_response) == "function")
assert(vectis.cai_worker == require("vectis.cai_worker"))
assert(vectis.cai_worker.REQUEST_KIND == "vectis.cai.request")
assert(vectis.cai_worker.REPLY_KIND == "vectis.cai.reply")
assert(type(vectis.cai_worker.request) == "function")
assert(type(vectis.cai_worker.decode_reply) == "function")
do
  local cai_worker_event = assert(vectis.cai_worker.request({
    provider = "openai",
    model = "gpt-test",
    text = "hello",
    developer_instructions = "be brief",
    max_output_tokens = 4,
  }))
  assert(cai_worker_event.kind == vectis.cai_worker.REQUEST_KIND)
  assert(cai_worker_event.expects_reply == true)
  assert(cai_worker_event.payload:find('"model":"gpt-test"', 1, true))
  local cai_worker_reply = assert(vectis.cai_worker.decode_reply({
    kind = vectis.cai_worker.REPLY_KIND,
    payload = '{"status":0,"source_code":0,"dependency_code":0,' ..
        '"http_status":0,"text":"worker ok"}',
  }))
  assert(cai_worker_reply.ok == true)
  assert(cai_worker_reply.status == status.OK)
  assert(cai_worker_reply.source_code == status.ERROR_SOURCE_NONE)
  assert(cai_worker_reply.text == "worker ok")
  local oversized_ok, oversized_err = pcall(vectis.cai_worker.request, {
    provider = "openai",
    model = "gpt-test",
    text = "hello",
    max_output_tokens = 2147483648,
  })
  assert(oversized_ok == false)
  assert(tostring(oversized_err):find("too large", 1, true))
end
assert(vectis.audio_worker == require("vectis.audio_worker"))
assert(vectis.audio_worker.DECODE_KIND == "vectis.audio.decode")
assert(vectis.audio_worker.ENCODE_KIND == "vectis.audio.encode")
assert(vectis.audio_worker.VOX_KIND == "vectis.audio.vox")
assert(vectis.audio_worker.REPLY_KIND == "vectis.audio.reply")
assert(vectis.audio_worker.VOX_STATE_KIND == "vectis.audio.vox.state")
assert(vectis.audio_worker.VOX_SEGMENT_KIND == "vectis.audio.vox.segment")
assert(type(vectis.audio_worker.decode_file_request) == "function")
assert(type(vectis.audio_worker.encode_file_request) == "function")
assert(type(vectis.audio_worker.vox_request) == "function")
assert(type(vectis.audio_worker.decode_reply) == "function")
assert(type(vectis.audio_worker.decode_vox_state) == "function")
assert(type(vectis.audio_worker.decode_vox_segment) == "function")
do
  local decode_event = assert(vectis.audio_worker.decode_file_request({
    path = "input.wav",
    encoding = "wav",
    max_frames = 128,
  }))
  assert(decode_event.kind == vectis.audio_worker.DECODE_KIND)
  assert(decode_event.expects_reply == true)
  assert(decode_event.payload:find('"path":"input.wav"', 1, true))
  local encode_event = assert(vectis.audio_worker.encode_file_request({
    path = "output.wav",
    format = "wav",
    frames = { 0.0, 0.25, -0.25 },
  }))
  assert(encode_event.kind == vectis.audio_worker.ENCODE_KIND)
  assert(encode_event.expects_reply == true)
  assert(encode_event.payload:find('"frames"', 1, true))
  local audio_reply = assert(vectis.audio_worker.decode_reply({
    kind = vectis.audio_worker.REPLY_KIND,
    payload = '{"status":0,"source_code":0,"dependency_code":0,' ..
        '"operation":"decode","path":"input.wav","sample_rate":16000,' ..
        '"channels":1,"frames":[0.25,-0.25]}',
  }))
  assert(audio_reply.ok == true)
  assert(audio_reply.operation == "decode")
  assert(audio_reply.sample_rate == 16000)
  assert(audio_reply.channels == 1)
  assert(audio_reply.frame_count == 2)
  assert(audio_reply.frames[1] > 0.24 and audio_reply.frames[1] < 0.26)
  local vox_event = assert(vectis.audio_worker.vox_request({
    frames = { 0.0, 0.25, -0.25 },
    threshold = 0.01,
    release_silence_ms = 1,
    min_segment_ms = 1,
    max_segment_frames = 128,
  }))
  assert(vox_event.kind == vectis.audio_worker.VOX_KIND)
  assert(vox_event.expects_reply == true)
  assert(vox_event.payload:find('"frames"', 1, true))
  local vox_state = assert(vectis.audio_worker.decode_vox_state({
    kind = vectis.audio_worker.VOX_STATE_KIND,
    payload = '{"state":1,"segment_index":2,"threshold":0.25}',
  }))
  assert(vox_state.state == 1)
  assert(vox_state.segment_index == 2)
  assert(vox_state.threshold > 0.24 and vox_state.threshold < 0.26)
  local vox_segment = assert(vectis.audio_worker.decode_vox_segment({
    kind = vectis.audio_worker.VOX_SEGMENT_KIND,
    payload = '{"segment_index":2,"t0":0,"t1":10,"hard_cut":0,' ..
        '"is_final":1,"frames":[0.5,-0.5]}',
  }))
  assert(vox_segment.segment_index == 2)
  assert(vox_segment.is_final == true)
  assert(vox_segment.frame_count == 2)
  assert(vox_segment.frames[1] > 0.49 and vox_segment.frames[1] < 0.51)
end
assert(vectis.sus_worker == require("vectis.sus_worker"))
assert(vectis.sus_worker.TRANSCRIBE_PCM_KIND == "vectis.sus.transcribe_pcm")
assert(vectis.sus_worker.TRANSCRIBE_FILE_KIND == "vectis.sus.transcribe_file")
assert(vectis.sus_worker.REPLY_KIND == "vectis.sus.reply")
assert(type(vectis.sus_worker.transcribe_pcm_request) == "function")
assert(type(vectis.sus_worker.transcribe_file_request) == "function")
assert(type(vectis.sus_worker.decode_reply) == "function")
do
  local pcm_event = assert(vectis.sus_worker.transcribe_pcm_request({
    frames = { 0.0, 0.25, -0.25 },
    language = "en",
    max_text_bytes = 4096,
  }))
  assert(pcm_event.kind == vectis.sus_worker.TRANSCRIBE_PCM_KIND)
  assert(pcm_event.expects_reply == true)
  assert(pcm_event.payload:find('"frames"', 1, true))
  assert(pcm_event.payload:find('"language":"en"', 1, true))
  local file_event = assert(vectis.sus_worker.transcribe_file_request({
    path = "input.wav",
    encoding = "wav",
    output = "file",
    output_path = "transcript.txt",
  }))
  assert(file_event.kind == vectis.sus_worker.TRANSCRIBE_FILE_KIND)
  assert(file_event.expects_reply == true)
  assert(file_event.payload:find('"path":"input.wav"', 1, true))
  assert(file_event.payload:find('"output":"file"', 1, true))
  local sus_reply = assert(vectis.sus_worker.decode_reply({
    kind = vectis.sus_worker.REPLY_KIND,
    payload = '{"status":0,"source_code":0,"dependency_code":0,' ..
        '"operation":"transcribe_pcm","text":"hello world",' ..
        '"path":"input.wav","output_path":"transcript.txt"}',
  }))
  assert(sus_reply.ok == true)
  assert(sus_reply.operation == "transcribe_pcm")
  assert(sus_reply.text == "hello world")
  assert(sus_reply.path == "input.wav")
  assert(sus_reply.output_path == "transcript.txt")
end
assert(vectis.cert == cert)
assert(vectis.embedded == embedded)
assert(vectis.app == app_module)
assert(vectis.server == nil)
assert(vectis.ssh == ssh)
assert(vectis.kore == package.loaded["vectis.kore"])
assert(vectis.kore.runtime_available == true)
assert(vectis.kore.runtime_model == "embedded")
assert(vectis.kore.MAX_WORKER_COUNT == 253)
assert(vectis.kore.MAX_KORE_CURL_TIMEOUT_SECONDS == 65535)
assert(vectis.kore.DEFAULT_WEBSOCKET_MAX_FRAME_BYTES == 16384)
assert(vectis.kore.DEFAULT_WEBSOCKET_TIMEOUT_MS == 120000)
assert(vectis.kore.WORKER_DEATH_RESTART == 0)
assert(vectis.kore.WORKER_DEATH_TERMINATE == 1)
assert(vectis.kore.websocket.TEXT == vectis.websocket.TEXT)
assert(vectis.kore.websocket.BINARY == vectis.websocket.BINARY)
assert(type(embedded.has_assets) == "function")
assert(type(embedded.default_extract_policy) == "function")
assert(type(embedded.tree_sha256) == "function")
assert(type(embedded.read) == "function")
assert(type(embedded.stat) == "function")
assert(type(embedded.chunks) == "function")
assert(type(embedded.list) == "function")
assert(type(embedded.extract) == "function")
assert(vectis.log == log)
assert(vectis.cai == vcai)
assert(vcai.native == cai)
assert(vcai.tool_schema == cai.tool_schema)
assert(vcai.response_params == cai.response_params)
assert(vcai.model_info == cai.model_info)
assert(vcai.mcp_client == cai.mcp_client)
local vcai_config = vcai.config({
  provider = "openrouter",
  api_key_env = "VECTIS_TEST_CAI_KEY",
  auth_json_path = "auth.json",
  usage_limits = { max_total_tokens = 16 },
  agent_config = { model = "openai/gpt-oss-20b" },
})
assert(vcai_config.provider == nil)
assert(vcai_config.openrouter == true)
assert(vcai_config.api_key_env == "VECTIS_TEST_CAI_KEY")
assert(vcai_config.auth_json_path == nil)
assert(vcai_config.chatgpt_auth_json == "auth.json")
assert(vcai_config.usage_limits.max_total_tokens == 16)
local cai_err = vcai.error({ dependency_code = 7, http_status = 429 },
                           "rate limited")
assert(cai_err.status == status.ERR_STATE)
assert(cai_err.source_code == status.ERROR_SOURCE_CAI)
assert(cai_err.source == "cai")
assert(cai_err.dependency_code == 7)
assert(cai_err.http_status == 429)
local fake_agent = { closed = false }
function fake_agent:close()
  self.closed = true
end
function fake_agent:send_text(text)
  return { text = text }
end
local fake_client = { closed = false, created = false }
function fake_client:close()
  self.closed = true
end
function fake_client:new_agent(agent_config)
  self.created = agent_config and agent_config.model == "fake-model"
  return fake_agent
end
local client_result = assert(vcai.with_client({ client = fake_client },
                                              function(client)
  assert(client == fake_client)
  return "borrowed"
end))
assert(client_result == "borrowed")
assert(fake_client.closed == false)
local agent_result = assert(vcai.with_agent({
  client = fake_client,
  agent_config = { model = "fake-model" },
}, function(agent)
  assert(agent == fake_agent)
  return "agent"
end))
assert(agent_result == "agent")
assert(fake_client.created == true)
assert(fake_agent.closed == true)
assert(type(ssh) == "table")
assert(type(ssh.open) == "function")
assert(type(ssh.exec) == "function")
assert(type(ssh.sftp_upload_file) == "function")
assert(type(ssh.sftp_download_file) == "function")
assert(type(ssh.sftp_open) == "function")
assert(ssh.SFTP_OPEN_READ == 0x01)
assert(ssh.SFTP_OPEN_WRITE == 0x02)
assert(ssh.SFTP_OPEN_CREATE == 0x04)
assert(ssh.SFTP_OPEN_TRUNCATE == 0x08)
assert(ssh.SFTP_OPEN_APPEND == 0x10)
assert(type(ssh.scp_upload_file) == "function")
assert(type(ssh.scp_download_file) == "function")
assert(type(ssh.sftp_stat) == "function")
assert(type(ssh.sftp_mkdir) == "function")
assert(type(ssh.sftp_remove) == "function")
assert(type(ssh.sftp_rmdir) == "function")
assert(type(ssh.sftp_rename) == "function")
assert(type(ssh.sftp_chmod) == "function")
local smtp = require("vectis.smtp")
assert(type(smtp.send) == "function")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")
assert(package.loaded.luarocks == nil)
assert(not package.path:lower():find("luarocks", 1, true))
assert(not package.cpath:lower():find("luarocks", 1, true))
assert(package.loaded.vectis == vectis)
assert(package.loaded.lockdc == lockdc)
assert(package.loaded.lonejson == lonejson)
assert(package.loaded.cai == cai)
assert(package.loaded.lql == lql)
assert(package.loaded["lql.core"] == lql.core)
assert(package.loaded.pslog == pslog)
assert(package.loaded["vectis.mailbox"] == mailbox)
assert(package.loaded["vectis.curl_worker"] == vectis.curl_worker)
assert(package.loaded["vectis.cai_worker"] == vectis.cai_worker)
assert(package.loaded["vectis.audio_worker"] == vectis.audio_worker)
assert(package.loaded["vectis.sus_worker"] == vectis.sus_worker)
assert(package.loaded["vectis.log"] == log)
assert(package.loaded.libmdf == libmdf)
assert(package.loaded.softline == softline)
assert(package.loaded.curl == curl)
assert(package.loaded.openssl == openssl)
assert(package.loaded.zlib == zlib)
assert(package.loaded.opcua == opcua)
assert(package.loaded.audio == audio)
assert(package.loaded.sus == sus)
assert(package.loaded["vectis.auth"] == auth)
assert(package.loaded["vectis.auth.core"] == auth_core)
assert(package.loaded["vectis.cert"] == cert)
assert(package.loaded["vectis.embedded"] == embedded)
assert(package.loaded["vectis.app"] == app_module)
assert(package.loaded["vectis.ssh"] == ssh)
assert(package.loaded["vectis.kore"] == vectis.kore)
assert(package.loaded["vectis.status"] == status)
assert(package.loaded["vectis.rest"] == rest)
assert(vectis.rest == rest)
assert(package.loaded["vectis.terminal"] == terminal)
assert(vectis.terminal == terminal)
assert(package.loaded["vectis.webdav"] == webdav)
assert(vectis.webdav == webdav)
assert(package.loaded["vectis.mqtt"] == mqtt)
assert(vectis.mqtt == mqtt)
assert(package.loaded["vectis.http"] == http)
assert(vectis.http == http)
assert(package.loaded["vectis.cai"] == vcai)
assert(vectis.cai == vcai)
assert(package.loaded["vectis.smtp"] == smtp)
assert(vectis.smtp == smtp)
assert(package.loaded["vectis.lockd"] == lockd)
assert(vectis.lockd == lockd)
assert(package.loaded["vectis.dsv"] == dsv)
assert(vectis.dsv == dsv)
assert(package.loaded["vectis.xml"] == xml)
assert(vectis.xml == xml)

local box = mailbox.new({capacity = 2, max_payload_bytes = 32})
assert(type(box.publish) == "function")
assert(type(mailbox.broker) == "function")
assert(box:depth() == 0)
local box_stats = box:stats()
assert(box_stats.capacity == 2)
assert(box_stats.max_payload_bytes == 32)
assert(box_stats.depth == 0)
assert(box_stats.high_water_depth == 0)
assert(box:publish({kind = "opcua.value", payload = "hello", correlation_id = 7, expects_reply = true}) == true)
assert(box:depth() == 1)
box_stats = box:stats()
assert(box_stats.published == 1)
assert(box_stats.depth == 1)
assert(box_stats.high_water_depth == 1)
local event = assert(box:next(0))
assert(event.kind == "opcua.value")
assert(event.payload == "hello")
assert(event.correlation_id == 7)
assert(event.expects_reply == true)
assert(box:depth() == 0)
box_stats = box:stats()
assert(box_stats.drained == 1)
assert(box:publish({kind = "one", payload = "1"}) == true)
assert(box:publish({kind = "two", payload = "2"}) == true)
local ok, full_error = box:publish({kind = "three", payload = "3"})
assert(ok == nil)
assert(full_error.status == vectis.ERR_CONFLICT)
box_stats = box:stats()
assert(box_stats.full_failures == 1)
assert(box_stats.publish_failures == 1)
assert(box_stats.high_water_depth == 2)
assert(assert(box:next(0)).kind == "one")
assert(assert(box:next(0)).kind == "two")
local none, timeout_error = box:next(0)
assert(none == nil)
assert(timeout_error.status == vectis.ERR_TIMEOUT)
box_stats = box:stats()
assert(box_stats.timeout_failures == 1)

local request_id = assert(box:request({kind = "worker.opcua", payload = "read"}))
assert(type(request_id) == "number")
box_stats = box:stats()
assert(box_stats.requests_published == 1)
assert(box_stats.correlation_ids_issued == 1)
event = assert(box:next(0))
assert(event.kind == "worker.opcua")
assert(event.correlation_id == request_id)
assert(event.expects_reply == true)
assert(box:reply(request_id, {kind = "worker.result", payload = "ok"}) == true)
box_stats = box:stats()
assert(box_stats.replies_published == 1)
event = assert(box:next(0))
assert(event.kind == "worker.result")
assert(event.payload == "ok")
assert(event.correlation_id == request_id)
assert(event.expects_reply == false)

assert(box:publish({kind = "pump.a", payload = "a"}) == true)
assert(box:publish({kind = "pump.b", payload = "b"}) == true)
local pumped = {}
local pumped_count = assert(box:pump(function(item)
  pumped[#pumped + 1] = item.kind .. ":" .. item.payload
end, {max = 4, timeout_ms = 0}))
assert(pumped_count == 2)
assert(table.concat(pumped, ",") == "pump.a:a,pump.b:b")
box_stats = box:stats()
assert(box_stats.pump_calls == 1)
assert(box_stats.pump_events == 2)
assert(box_stats.pump_callback_failures == 0)
assert(box:publish({kind = "pump.fail", payload = "x"}) == true)
local pump_ok, pump_error = box:pump(function()
  error("intentional pump failure")
end, {max = 1, timeout_ms = 0})
assert(pump_ok == nil)
assert(pump_error.status == vectis.ERR_STATE)
box_stats = box:stats()
assert(box_stats.pump_calls == 2)
assert(box_stats.pump_events == 3)
assert(box_stats.pump_callback_failures == 1)
assert(box:close() == true)
event, timeout_error = box:next(0)
assert(event == nil)
assert(timeout_error.status == vectis.ERR_STATE)

local broker_requests = mailbox.new({capacity = 2, max_payload_bytes = 64})
local broker = assert(mailbox.broker({
  requests = broker_requests,
  reply = {max_payload_bytes = 64},
  max_pending = 2,
}))
local broker_reply, broker_error = broker:request({
  kind = "route.worker",
  payload = "read",
}, {timeout_ms = 1})
assert(broker_reply == nil)
assert(broker_error.status == vectis.ERR_TIMEOUT)
local broker_request = assert(broker_requests:next(0))
assert(broker_request.kind == "route.worker")
assert(broker_request.expects_reply == true)
local late_ok, late_error = broker:reply(broker_request.correlation_id, {
  kind = "worker.result",
  payload = "late",
})
assert(late_ok == nil)
assert(late_error.status == vectis.ERR_TIMEOUT)
assert(broker:close() == true)
broker_reply, broker_error = broker:request({
  kind = "route.worker",
  payload = "after-close",
}, {timeout_ms = 0})
assert(broker_reply == nil)
assert(broker_error.status == vectis.ERR_STATE)
assert(broker_requests:close() == true)

assert(type(dsv.parse) == "function")
assert(type(dsv.parse_json) == "function")
assert(type(dsv.each) == "function")
assert(type(dsv.to_string) == "function")
assert(type(xml.parse) == "function")
assert(type(xml.parse_record) == "function")
assert(type(xml.serialize) == "function")
assert(type(rest.route) == "function")
assert(type(rest.client) == "function")
local smoke_rest_client = rest.client({base_url = "http://127.0.0.1"})
assert(type(smoke_rest_client.head) == "function")
assert(type(smoke_rest_client.options) == "function")
local smoke_http = http
assert(type(smoke_http.options) == "function")
assert(type(smoke_http.options_json) == "function")
assert(type(smoke_http.download_file) == "function")
assert(type(smoke_http.upload_file) == "function")
assert(type(smoke_http.sftp_download_file) == "function")
assert(type(smoke_http.sftp_upload_file) == "function")
assert(type(smoke_http.client({}).options) == "function")
assert(type(smoke_http.client({}).options_json) == "function")
assert(type(smoke_http.client({}).download_file) == "function")
assert(type(smoke_http.client({}).upload_file) == "function")
assert(type(smoke_http.client({}).sftp_download_file) == "function")
assert(type(smoke_http.client({}).sftp_upload_file) == "function")
assert(type(terminal.markdown) == "function")
assert(type(terminal.markdown_stream) == "function")
assert(type(terminal.editor) == "function")
assert(type(webdav.request) == "function")
assert(type(webdav.propfind) == "function")
assert(type(webdav.mkcol) == "function")
assert(type(webdav.copy) == "function")
assert(type(webdav.move) == "function")
assert(type(mqtt.publish) == "function")
assert(type(openssl.version) == "function")
assert(type(openssl.sha256_hex) == "function")
assert(type(openssl.hmac_sha256_hex) == "function")
assert(type(openssl.digest) == "function")
assert(type(openssl.digest_hex) == "function")
assert(type(openssl.hmac) == "function")
assert(type(openssl.hmac_hex) == "function")
assert(type(openssl.random_bytes) == "function")
assert(type(zlib.version) == "function")
assert(type(zlib.deflate) == "function")
assert(type(zlib.inflate) == "function")
assert(type(zlib.compress) == "function")
assert(type(zlib.decompress) == "function")
assert(type(zlib.gzip) == "function")
assert(type(zlib.gunzip) == "function")
assert(type(zlib.deflate_file) == "function")
assert(type(zlib.inflate_file) == "function")
assert(type(zlib.compress_file) == "function")
assert(type(zlib.decompress_file) == "function")
assert(type(zlib.gzip_file) == "function")
assert(type(zlib.gunzip_file) == "function")
local xml_schema = lonejson.schema("invoice", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("amount", lonejson.object({
    required = true,
    fields = {
      lonejson.field("currency", lonejson.string({required = true})),
      lonejson.field("text", lonejson.f64({required = true})),
    },
  })),
  lonejson.field("line", lonejson.object_array({
    fields = {
      lonejson.field("sku", lonejson.string({required = true})),
      lonejson.field("quantity", lonejson.i64({required = true})),
    },
  })),
  lonejson.field("tag", lonejson.string_array()),
  lonejson.field("active", lonejson.boolean({required = true})),
})
local xml_payload = table.concat({
  "<invoice id=\"inv-1\">",
  "<amount currency=\"EUR\">12.50</amount>",
  "<line><sku>A-1</sku><quantity>2</quantity></line>",
  "<line><sku>B-2</sku><quantity>5</quantity></line>",
  "<tag>paid</tag><tag>priority</tag>",
  "<active>true</active>",
  "</invoice>",
})
local xml_doc, xml_err = xml.parse({
  schema = xml_schema,
  xml = xml_payload,
  root_element = "invoice",
  trim_text = true,
})
assert(xml_doc, xml_err and xml_err.message)
assert(xml_doc.id == "inv-1")
assert(xml_doc.amount.currency == "EUR")
assert(xml_doc.amount.text == 12.5)
assert(#xml_doc.line == 2)
assert(xml_doc.line[1].sku == "A-1")
assert(xml_doc.line[2].quantity == 5)
assert(xml_doc.tag[2] == "priority")
assert(xml_doc.active == true)
xml_serialized = assert(xml.serialize({
  schema = xml_schema,
  value = {
    id = "inv-&-1",
    amount = {currency = "EUR", text = 12.5},
    line = {
      {sku = "A-1", quantity = 2},
      {sku = "B-2", quantity = 5},
    },
    tag = {"paid", "priority"},
    active = true,
  },
  root_element = "invoice",
  trim_text = true,
}))
assert(xml_serialized:find("<invoice>", 1, true))
assert(xml_serialized:find("<id>inv-&amp;-1</id>", 1, true))
assert(xml_serialized:find("<line><sku>A-1</sku><quantity>2</quantity></line>", 1, true))
xml_roundtrip = assert(xml.parse({
  schema = xml_schema,
  xml = xml_serialized,
  root_element = "invoice",
  trim_text = true,
}))
assert(xml_roundtrip.id == "inv-&-1")
assert(xml_roundtrip.line[2].quantity == 5)
xml_attr_schema = lonejson.schema("item", {
  lonejson.field("@id", lonejson.string({required = true})),
  lonejson.field("text", lonejson.string({required = true})),
})
xml_attr_serialized = assert(xml.serialize({
  schema = xml_attr_schema,
  value = {["@id"] = "a&b", text = "hello <xml>"},
  root_element = "item",
  attribute_prefix = "@",
}))
assert(xml_attr_serialized == "<item id=\"a&amp;b\">hello &lt;xml&gt;</item>")
xml_attr_roundtrip = assert(xml.parse({
  schema = xml_attr_schema,
  xml = xml_attr_serialized,
  root_element = "item",
  attribute_prefix = "@",
}))
assert(xml_attr_roundtrip["@id"] == "a&b")
assert(xml_attr_roundtrip.text == "hello <xml>")
local xml_path = os.tmpname()
local xml_file = assert(io.open(xml_path, "wb"))
xml_file:write(xml_payload)
xml_file:close()
local xml_record = assert(xml.parse_record({
  schema = xml_schema,
  path = xml_path,
  root_element = "invoice",
  trim_text = true,
}))
os.remove(xml_path)
local xml_record_table = xml_record:to_table()
assert(xml_record_table.line[2].sku == "B-2")
local bad_xml_doc, bad_xml_err = xml.parse({
  schema = xml_schema,
  xml = "<wrong/>",
  root_element = "invoice",
})
assert(bad_xml_doc == nil)
assert(type(bad_xml_err) == "table")
assert(bad_xml_err.message:match("root"))

assert(type(opcua.open62541_version()) == "string")
assert(#opcua.open62541_version() > 0)
assert(type(opcua.facade_version()) == "string")
assert(#opcua.facade_version() > 0)
assert(type(opcua.result_string(opcua.OK)) == "string")
assert(type(opcua.status_name(0)) == "string")
assert(type(opcua.STATUS_BAD_USER_ACCESS_DENIED) == "number")
assert(type(opcua.server) == "function")
assert(opcua.NODE_CLASS_OBJECT == 1)
assert(opcua.NODE_CLASS_VARIABLE == 2)
assert(opcua.NODE_OBJECTS_FOLDER == 85)
assert(opcua.NODE_BASE_EVENT_TYPE == 2041)
assert(opcua.REFERENCE_ORGANIZES == 35)
local bad_node, bad_node_err = opcua.node_id_parse("not-a-node-id")
assert(bad_node == nil)
assert(bad_node_err.status == vectis.ERR_INVALID)
assert(bad_node_err.status_string == "invalid")
assert(bad_node_err.source == "cpkt")
assert(bad_node_err.source_code == vectis.ERROR_SOURCE_CPKT)
assert(bad_node_err.dependency == "opcua")
assert(bad_node_err.dependency_code == bad_node_err.result)
assert(bad_node_err.opcua_status ~= nil)
assert(bad_node_err.status_name == nil)
assert(type(audio.result_string(audio.OK)) == "string")
assert(audio.can_decode("wav") == true)
assert(audio.can_encode("wav") == true)
assert(type(audio.capture.open_default) == "function")
assert(type(audio.playback.open_default) == "function")
assert(type(sus.facade_version()) == "string")
assert(sus.model_catalog_count() > 0)
local sus_registry = debug.getregistry()
assert(type(sus_registry["sus.model"].__index.create_transcriber) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k_text) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_decoder_segmented) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_decoder_segmented_text) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_audio_vox_segment) == "function")
assert(type(sus_registry["sus.transcriber"].__index.revised_text) == "function")
local numeric_node = opcua.node_id_numeric(2, 1234)
assert(numeric_node:type() == opcua.NODE_ID_NUMERIC)
assert(numeric_node:namespace() == 2)
assert(opcua.node_id_parse(tostring(numeric_node)) == numeric_node)
local string_node = opcua.node_id_string(3, "temperature")
assert(string_node:type() == opcua.NODE_ID_STRING)
assert(opcua.node_id(tostring(string_node)) == string_node)
local guid_node = opcua.node_id_guid(1, "01234567-89ab-cdef-0123-456789abcdef")
assert(guid_node:type() == opcua.NODE_ID_GUID)
local expanded_node = opcua.expanded_node_id_local(string_node)
assert(expanded_node:node_id() == string_node)
assert(expanded_node:server_index() == 0)
assert(expanded_node:namespace_uri() == nil)
assert(opcua.expanded_node_id_parse(tostring(expanded_node)) == expanded_node)
local expanded_uri_node =
    opcua.expanded_node_id_uri("urn:vectis:smoke", string_node)
assert(expanded_uri_node:namespace_uri() == "urn:vectis:smoke")
local expanded_server_node = opcua.expanded_node_id_server(7, string_node)
assert(expanded_server_node:server_index() == 7)
local expanded_server_uri_node =
    opcua.expanded_node_id_server_uri(9, "urn:vectis:smoke", string_node)
assert(expanded_server_uri_node:server_index() == 9)
assert(expanded_server_uri_node:namespace_uri() == "urn:vectis:smoke")
local boolean_value = opcua.value_boolean(true)
assert(boolean_value:type() == opcua.VALUE_BOOLEAN)
assert(boolean_value:get() == true)
local string_value = opcua.value_string("hello")
assert(string_value:type() == opcua.VALUE_STRING)
assert(string_value:get() == "hello")
local integer_array_value = opcua.value_integer_array({ 1, 2 })
assert(integer_array_value:type() == opcua.VALUE_INTEGER_ARRAY)
assert(integer_array_value:get()[2] == 2)
local string_array_value = opcua.value_string_array({ "a", "b" })
assert(string_array_value:type() == opcua.VALUE_STRING_ARRAY)
assert(string_array_value:get()[1] == "a")
local guid_array_value =
    opcua.value_guid_array({ "00112233-4455-6677-8899-aabbccddeeff" })
assert(guid_array_value:type() == opcua.VALUE_GUID_ARRAY)
assert(guid_array_value:get()[1] == "00112233-4455-6677-8899-aabbccddeeff")
assert(opcua.NODE_CLASS_VARIABLE == 2)
assert(opcua.BROWSE_FORWARD == 0)
assert(opcua.BROWSE_RESULT_ALL >= opcua.BROWSE_RESULT_DISPLAY_NAME)
assert(opcua.MONITORING_DISABLED == 0)
assert(opcua.MONITORING_REPORTING == 2)
assert(opcua.DEADBAND_NONE == 0)
assert(opcua.METHOD_ARGUMENT_INPUT == 0)
assert(opcua.METHOD_ARGUMENT_OUTPUT == 1)
local opcua_registry = debug.getregistry()
assert(type(opcua_registry["opcua.server"].__index.read_data_value) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_integer_array_range) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_string_array) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_localized_text_array) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_method) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_method_many) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_method_argument_count) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_method_argument) == "function")
assert(type(opcua_registry["opcua.server"].__index.set_default_security) == "function")
assert(type(opcua_registry["opcua.server"].__index.set_access_control) == "function")
assert(type(opcua_registry["opcua.server"].__index.browse_children) == "function")
assert(type(opcua_registry["opcua.server"].__index.browse_children_ex) == "function")
assert(type(opcua_registry["opcua.server"].__index.browse_children_page) == "function")
assert(type(opcua_registry["opcua.server"].__index.browse_next) == "function")
assert(type(opcua_registry["opcua.server"].__index.translate_browse_path) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_mqtt_pubsub_connection) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_published_dataset) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_published_variable) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_pubsub_writer_group) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_pubsub_data_set_writer) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_pubsub_reader_group) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_pubsub_data_set_reader) == "function")
assert(type(opcua_registry["opcua.server"].__index.write_pubsub_configuration) == "function")
assert(type(opcua_registry["opcua.server"].__index.load_pubsub_configuration) == "function")
assert(type(opcua_registry["opcua.server"].__index.add_reference_ex) == "function")
assert(type(opcua_registry["opcua.server"].__index.delete_reference_ex) == "function")
assert(type(opcua_registry["opcua.server"].__index.read_array_dimensions) == "function")
assert(type(opcua_registry["opcua.server"].__index.write_array_dimensions) == "function")
assert(type(opcua_registry["opcua.server"].__index.create_event) == "function")
assert(type(opcua_registry["opcua.server"].__index.trigger_event) == "function")
local client = assert(opcua.client())
assert(type(client.connect) == "function")
assert(type(client.set_default_encryption) == "function")
assert(type(client.create_subscription) == "function")
assert(type(client.modify_subscription) == "function")
assert(type(client.delete_subscription) == "function")
assert(type(client.monitor_value) == "function")
assert(type(client.monitor_value_ex) == "function")
assert(type(client.set_monitoring_mode) == "function")
assert(type(client.delete_monitored_item) == "function")
assert(type(client.monitor_events) == "function")
assert(type(client.monitor_event_fields) == "function")
assert(type(client.read_async) == "function")
assert(type(client.write_async) == "function")
assert(type(client.browse_children_async) == "function")
assert(type(client.call_method_async) == "function")
assert(type(client.add_object_async) == "function")
assert(type(client.add_variable_async) == "function")
assert(type(client.read) == "function")
assert(type(client.add_object) == "function")
assert(type(client.add_variable_under) == "function")
assert(type(client.add_object_type) == "function")
assert(type(client.add_variable_type) == "function")
assert(type(client.add_reference_type) == "function")
assert(type(client.add_data_type) == "function")
assert(type(client.add_view) == "function")
assert(type(client.add_reference) == "function")
assert(type(client.add_reference_ex) == "function")
assert(type(client.delete_reference) == "function")
assert(type(client.delete_reference_ex) == "function")
assert(type(client.read_node_id) == "function")
assert(type(client.read_browse_name) == "function")
assert(type(client.read_display_name) == "function")
assert(type(client.read_description) == "function")
assert(type(client.write_display_name) == "function")
assert(type(client.write_description) == "function")
assert(type(client.read_write_mask) == "function")
assert(type(client.read_user_write_mask) == "function")
assert(type(client.write_write_mask) == "function")
assert(type(client.read_is_abstract) == "function")
assert(type(client.write_is_abstract) == "function")
assert(type(client.read_symmetric) == "function")
assert(type(client.write_symmetric) == "function")
assert(type(client.read_inverse_name) == "function")
assert(type(client.write_inverse_name) == "function")
assert(type(client.read_contains_no_loops) == "function")
assert(type(client.write_contains_no_loops) == "function")
assert(type(client.read_event_notifier) == "function")
assert(type(client.write_event_notifier) == "function")
assert(type(client.read_data_type) == "function")
assert(type(client.write_data_type) == "function")
assert(type(client.read_value_rank) == "function")
assert(type(client.write_value_rank) == "function")
assert(type(client.read_array_dimensions) == "function")
assert(type(client.write_array_dimensions) == "function")
assert(type(client.read_access_level) == "function")
assert(type(client.read_user_access_level) == "function")
assert(type(client.write_access_level) == "function")
assert(type(client.read_access_level_ex) == "function")
assert(type(client.write_access_level_ex) == "function")
assert(type(client.read_minimum_sampling_interval) == "function")
assert(type(client.write_minimum_sampling_interval) == "function")
assert(type(client.read_historizing) == "function")
assert(type(client.write_historizing) == "function")
assert(type(client.read_executable) == "function")
assert(type(client.read_user_executable) == "function")
assert(type(client.write_executable) == "function")
assert(type(client.read_data_value) == "function")
assert(type(client.history_read_raw) == "function")
assert(type(client.read_boolean_array_range) == "function")
assert(type(client.read_integer_array_range) == "function")
assert(type(client.read_double_array_range) == "function")
assert(type(client.read_string_array_range) == "function")
assert(type(client.read_byte_string_array_range) == "function")
assert(type(client.read_uint64_array_range) == "function")
assert(type(client.read_datetime_array_range) == "function")
assert(type(client.read_status_array_range) == "function")
assert(type(client.read_guid_array_range) == "function")
assert(type(client.read_qualified_name_array_range) == "function")
assert(type(client.read_localized_text_array_range) == "function")
assert(type(client.write_index_range) == "function")
assert(type(client.browse_children) == "function")
assert(type(client.browse_children_ex) == "function")
assert(type(client.browse_children_page) == "function")
assert(type(client.browse_next) == "function")
assert(type(client.read_method_argument_count) == "function")
assert(type(client.read_method_argument) == "function")
assert(type(client.call_method) == "function")
assert(type(client.call_method_many) == "function")
assert(type(client.translate_browse_path) == "function")
assert(type(client.endpoint_count) == "function")
assert(type(client.endpoint_url_at) == "function")
assert(type(client.endpoints) == "function")
assert(type(client.server_count) == "function")
assert(type(client.server_application_uri) == "function")
assert(type(client.server_application_name) == "function")
assert(type(client.find_servers) == "function")
local security_server = assert(opcua.server({}))
local security_ok, security_err = pcall(function()
  security_server:set_default_security({ certificate = "cert-only" })
end)
assert(not security_ok)
assert(tostring(security_err):find("certificate and private_key", 1, true))
assert(security_server:close() == true)
local encryption_ok, encryption_err = pcall(function()
  client:set_default_encryption({ private_key = "key-only" })
end)
assert(not encryption_ok)
assert(tostring(encryption_err):find("certificate and private_key", 1, true))
assert(client:close() == true)

local server
do
  local smoke_lockd_dir = os.tmpname()
  os.remove(smoke_lockd_dir)
  server = assert(vectis.app.new({
    app_name = "lua-smoke",
    port = 18080,
    lockd = {
      endpoints = {"pouch://" .. smoke_lockd_dir},
      pouch_crypto_generate_key_file = false,
      pouch_compression = "zlib",
    },
  }))
end
assert(type(server.static_directory) == "function")
assert(type(server.webdav) == "function")
assert(type(server.webdav_embedded) == "function")
assert(type(server.route) == "function")
assert(type(server.dsv) == "function")
assert(type(server.upload) == "function")
assert(type(server.mcp) == "function")
assert(type(server.sse) == "function")
assert(type(server.metrics) == "function")
assert(type(server.opcua_server_service) == "function")
assert(type(server.opcua_server_service_states) == "function")
assert(type(server.curl_worker_service) == "function")
assert(type(server.cai_worker_service) == "function")
assert(type(server.audio_worker_service) == "function")
assert(type(server.sus_worker_service) == "function")
assert(type(server.json) == "function")
assert(type(server.text) == "function")
assert(type(server.redirect) == "function")
assert(type(server.auth_json) == "function")
do
  local requests = assert(vectis.mailbox.new({ capacity = 4 }))
  local audio_events = assert(vectis.mailbox.new({ capacity = 4 }))
  assert(server:curl_worker_service({
    name = "lua-smoke-curl-worker",
    request_mailbox = requests,
    start = false,
    logger_disabled = true,
  }) == true)
  assert(server:cai_worker_service({
    name = "lua-smoke-cai-worker",
    request_mailbox = requests,
    start = false,
    logger_disabled = true,
  }) == true)
  assert(server:audio_worker_service({
    name = "lua-smoke-audio-worker",
    request_mailbox = requests,
    event_mailbox = audio_events,
    start = false,
    logger_disabled = true,
  }) == true)
  assert(server:sus_worker_service({
    name = "lua-smoke-sus-worker",
    request_mailbox = requests,
    start = false,
    logger_disabled = true,
  }) == true)
  local curl_states = server:curl_worker_service_states()
  local cai_states = server:cai_worker_service_states()
  local audio_states = server:audio_worker_service_states()
  local sus_states = server:sus_worker_service_states()
  assert(curl_states[1].name == "lua-smoke-curl-worker")
  assert(cai_states[1].name == "lua-smoke-cai-worker")
  assert(audio_states[1].name == "lua-smoke-audio-worker")
  assert(sus_states[1].name == "lua-smoke-sus-worker")
  assert(curl_states[1].start_requested == false)
  assert(cai_states[1].start_requested == false)
  assert(audio_states[1].start_requested == false)
  assert(sus_states[1].start_requested == false)
end
do
  local lifecycle_opcua_server = assert(opcua.server({}))
  assert(server:opcua_server_service({
    name = "lua-smoke-opcua",
    server = lifecycle_opcua_server,
    start = true,
    logger_disabled = true,
    wait_internal = false,
    max_wait_ms = 5,
  }) == true)
  local lifecycle_opcua_states = server:opcua_server_service_states()
  assert(#lifecycle_opcua_states == 1)
  assert(lifecycle_opcua_states[1].name == "lua-smoke-opcua")
  assert(lifecycle_opcua_states[1].declared == true)
  assert(lifecycle_opcua_states[1].start_requested == true)
  assert(lifecycle_opcua_states[1].materialized == false)
  local callback_opcua_server = assert(opcua.server({}))
  assert(callback_opcua_server:set_access_control({
    callback = function()
      return true
    end,
  }) == true)
  local callback_service, callback_service_err = server:opcua_server_service({
    name = "lua-callback-opcua",
    server = callback_opcua_server,
  })
  assert(callback_service == nil)
  assert(type(callback_service_err) == "table")
  assert(callback_service_err.status == vectis.ERR_INVALID)
  assert(callback_service_err.message:match("Lua callbacks"))
  assert(callback_opcua_server:close() == true)
  _G.__vectis_smoke_lifecycle_opcua_server = lifecycle_opcua_server
end
local mcp_bad, mcp_bad_err = server:mcp({
  path = "/mcp-bad",
  tools = {},
})
assert(mcp_bad == nil)
assert(type(mcp_bad_err) == "table")
assert(mcp_bad_err.status == vectis.ERR_INVALID)
assert(mcp_bad_err.message:match("tools"))
assert(server:metrics({
  path = "/lua-smoke-metrics",
  title = "lua smoke metrics",
}) == true)
assert(server:mcp({
  path = "/mcp",
  name = "lua-smoke-mcp",
  tools = {
    {
      name = "echo",
      description = "echo raw JSON arguments",
      schema_json = '{"type":"object","properties":{"text":{"type":"string"}}}',
      callback = function(arguments_json)
        return '{"content":[{"type":"text","text":' ..
            string.format("%q", arguments_json) .. '}]}'
      end,
    },
  },
}) == true)
_G.__vectis_mcp_session_bad, _G.__vectis_mcp_session_bad_err = server:mcp({
  path = "/mcp-session-bad",
  enable_sessions = true,
  tools = {
    {
      name = "session_bad",
      schema_json = '{"type":"object"}',
      callback = function()
        return '{"content":[]}'
      end,
    },
  },
})
assert(_G.__vectis_mcp_session_bad == nil)
assert(type(_G.__vectis_mcp_session_bad_err) == "table")
assert(_G.__vectis_mcp_session_bad_err.status == vectis.ERR_INVALID)
assert(_G.__vectis_mcp_session_bad_err.message:match("session"))
local route_auth_path = os.tmpname()
os.remove(route_auth_path)
local route_auth_state_path = os.tmpname()
os.remove(route_auth_state_path)
assert(vectis.auth.store_init({
  credentials_path = route_auth_path,
  auth_state_path = route_auth_state_path,
}))
assert(server:auth_routes({
  path_prefix = "/_auth-state",
  credentials_path = route_auth_path,
  auth_state_path = route_auth_state_path,
  realm = "lua-route-state",
}) == true)
do
  local consumer_cache_dir = os.tmpname()
  os.remove(consumer_cache_dir)
  assert(server:consumer_service({
    name = "lua-smoke-state",
    queue = "lua-smoke-state",
    owner = "lua-smoke",
    handler = {
      kind = "webdav_marker",
      cache_dir = consumer_cache_dir,
    },
  }) == true)
  local consumer_states = server:consumer_service_states()
  assert(type(consumer_states) == "table")
  assert(#consumer_states == 1)
  assert(consumer_states[1].name == "lua-smoke-state")
  assert(consumer_states[1].queue == "lua-smoke-state")
  assert(consumer_states[1].owner == "lua-smoke")
  assert(consumer_states[1].declared == true)
  assert(consumer_states[1].start_requested == true)
  assert(consumer_states[1].materialized == false)
  assert(consumer_states[1].started == false)
  assert(consumer_states[1].failed == false)
  assert(consumer_states[1].terminal_status == vectis.OK)
end
local consumer_service, consumer_service_error = server:consumer_service({
  queue = "lua-smoke",
  on_message = function() end,
})
assert(consumer_service == nil)
assert(type(consumer_service_error) == "table")
assert(consumer_service_error.status == vectis.ERR_INVALID)
assert(consumer_service_error.status_string == "invalid")
assert(consumer_service_error.message:match("direct Lua callbacks"))
consumer_service, consumer_service_error = server:consumer_service({
  queue = "lua-smoke",
  handler = {
    kind = "missing_receiver",
  },
})
assert(consumer_service == nil)
assert(type(consumer_service_error) == "table")
assert(consumer_service_error.status == vectis.ERR_INVALID)
assert(consumer_service_error.message:match("receiver_kind"))
server:close()
assert(_G.__vectis_smoke_lifecycle_opcua_server:close() == true)
_G.__vectis_smoke_lifecycle_opcua_server = nil

do
  local bad_shutdown_server, bad_shutdown_error = vectis.app.new({
    app_name = "lua-bad-shutdown-grace",
    port = 18160,
    shutdown_grace_ms = -1,
  })
  assert(bad_shutdown_server == nil)
  assert(type(bad_shutdown_error) == "table")
  assert(bad_shutdown_error.status == vectis.ERR_INVALID)
  assert(bad_shutdown_error.message:match("shutdown_grace_ms"))
end

do
  local bad_spool_server, bad_spool_error = vectis.app.new({
    app_name = "lua-bad-request-body-spool-dir",
    port = 18161,
    request_body_spool_dir = "",
  })
  assert(bad_spool_server == nil)
  assert(type(bad_spool_error) == "table")
  assert(bad_spool_error.status == vectis.ERR_INVALID)
  assert(bad_spool_error.message:match("request_body_spool_dir"))
end

do
  local production_profile_server = assert(vectis.app.new({
    app_name = "lua-production-profile",
    profile = "production_webserver",
    port = 18168,
  }))
  production_profile_server:close()
end

do
  local proxy_identity_server = assert(vectis.app.new({
    app_name = "lua-client-ip",
    port = 18169,
    client_ip = {trusted_proxies = {"127.0.0.1", "2001:db8::10"}},
  }))
  proxy_identity_server:close()
  local invalid_proxy_server, invalid_proxy_error = vectis.app.new({
    app_name = "lua-invalid-client-ip",
    port = 18170,
    client_ip = {trusted_proxies = {"not-an-ip"}},
  })
  assert(invalid_proxy_server == nil)
  assert(type(invalid_proxy_error) == "table")
  assert(invalid_proxy_error.message:find("trusted proxy", 1, true))
end

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-profile",
      profile = "debug-ish",
      port = 18167,
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("profile"))
end

do
  local worker_count_server = assert(vectis.app.new({
    app_name = "lua-worker-count",
    port = 18169,
    worker_count = 1,
    worker_accept_threshold = 4,
    worker_rlimit_nofiles = 1024,
    worker_set_affinity = false,
    worker_shutdown_timeout_ms = 2500,
    max_connections = 64,
    request_limit = 32,
    max_request_header_bytes = 2048,
    max_request_body_bytes = 4096,
    request_header_timeout_ms = 1000,
    request_body_idle_timeout_ms = 2000,
    response_write_idle_timeout_ms = 3000,
    request_body_min_rate_bytes_per_sec = 128,
    request_body_min_rate_grace_ms = 500,
    idle_timeout_ms = 4000,
    keepalive_disabled = true,
    keepalive_timeout_ms = 0,
    keepalive_max_requests = 0,
    kore_curl_timeout_seconds = 7,
    kore_curl_recv_max_bytes = 65536,
    kore_quiet = true,
    worker_death_policy = "terminate",
    socket_backlog = 128,
    request_process_budget_ms = 50,
    hsts_max_age_seconds = 0,
    websocket_max_frame_bytes = 8192,
    websocket_timeout_ms = 45000,
    server_header = "vectis-lua-smoke",
    access_log_path = "/tmp/vectis-lua-smoke-access.log",
    pretty_error_pages = true,
  }))
  worker_count_server:close()
end

do
  local bad_worker_count_server, bad_worker_count_error = vectis.app.new({
    app_name = "lua-bad-worker-count",
    port = 18173,
    worker_count = 256,
  })
  assert(bad_worker_count_server == nil)
  assert(type(bad_worker_count_error) == "table")
  assert(bad_worker_count_error.status == vectis.ERR_INVALID)
  assert(bad_worker_count_error.message:match("worker_count"))
end

do
  local bad_kore_curl_server, bad_kore_curl_error = vectis.app.new({
    app_name = "lua-bad-kore-curl-timeout",
    port = 18174,
    kore_curl_timeout_seconds = vectis.kore.MAX_KORE_CURL_TIMEOUT_SECONDS + 1,
  })
  assert(bad_kore_curl_server == nil)
  assert(type(bad_kore_curl_error) == "table")
  assert(bad_kore_curl_error.status == vectis.ERR_INVALID)
  assert(bad_kore_curl_error.message:match("kore_curl_timeout_seconds"))
end

do
  local bad_worker_shutdown_server, bad_worker_shutdown_error =
    vectis.app.new({
      app_name = "lua-bad-worker-shutdown-timeout",
      port = 18174,
      worker_shutdown_timeout_ms = -1,
    })
  assert(bad_worker_shutdown_server == nil)
  assert(type(bad_worker_shutdown_error) == "table")
  assert(bad_worker_shutdown_error.status == vectis.ERR_INVALID)
  assert(bad_worker_shutdown_error.message:match("worker_shutdown_timeout_ms"))
end

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-worker-death-policy",
      port = 18175,
      worker_death_policy = "replace",
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("worker_death_policy"))
end

do
  local direct_policy_server = assert(vectis.app.new({
    app_name = "lua-direct-supervision-policy",
    port = 18162,
    supervision_policy = "direct",
  }))
  direct_policy_server:close()
end

do
  local supervised_policy_server = assert(vectis.app.new({
    app_name = "lua-supervised-supervision-policy",
    port = 18163,
    supervision_policy = "supervised",
  }))
  supervised_policy_server:close()
end

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-supervision-policy",
      port = 18168,
      supervision_policy = "invalid",
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("supervision_policy"))
end

do
  local service_policy_server = assert(vectis.app.new({
    app_name = "lua-service-failure-continue-policy",
    port = 18164,
    service_failure_policy = "continue",
  }))
  service_policy_server:close()
end

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-service-failure-policy",
      port = 18165,
      service_failure_policy = "invalid",
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("service_failure_policy"))
end

do
  local quiescence_policy_server = assert(vectis.app.new({
    app_name = "lua-quiescence-warn-unavailable-policy",
    port = 18166,
    quiescence_policy = "warn_unavailable",
  }))
  quiescence_policy_server:close()
end

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-quiescence-policy",
      port = 18167,
      quiescence_policy = "invalid",
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("quiescence_policy"))
end

local tls_bundle_pem_server = assert(vectis.app.new({
  app_name = "lua-manual-tls-bundle-pem",
  port = 18170,
  tls = {
    mode = "manual",
    version = "both",
    cert_key_bundle_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n-----BEGIN PRIVATE KEY-----\n" ..
        "placeholder\n-----END PRIVATE KEY-----\n",
  },
}))
tls_bundle_pem_server:close()

local tls_split_pem_server = assert(vectis.app.new({
  app_name = "lua-manual-tls-split-pem",
  port = 18171,
  tls = {
    mode = "manual",
    certificate_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n",
    private_key_pem = "-----BEGIN PRIVATE KEY-----\nplaceholder\n" ..
        "-----END PRIVATE KEY-----\n",
    ca_bundle_pem = "-----BEGIN CERTIFICATE-----\nca\n" ..
        "-----END CERTIFICATE-----\n",
  },
}))
tls_split_pem_server:close()

local tls_client_ca_pem_server = assert(vectis.app.new({
  app_name = "lua-manual-tls-client-ca-pem",
  port = 18172,
  tls = {
    mode = "manual",
    cert_key_bundle_pem = "-----BEGIN CERTIFICATE-----\nplaceholder\n" ..
        "-----END CERTIFICATE-----\n-----BEGIN PRIVATE KEY-----\n" ..
        "placeholder\n-----END PRIVATE KEY-----\n",
    require_client_certificate = true,
    client_ca_bundle_pem = "-----BEGIN CERTIFICATE-----\nclient-ca\n" ..
        "-----END CERTIFICATE-----\n",
  },
}))
tls_client_ca_pem_server:close()

do
  local ok, err = pcall(function()
    return vectis.app.new({
      app_name = "lua-bad-tls-version",
      port = 18175,
      tls = {
        mode = "manual",
        version = "ssl3",
      },
    })
  end)
  assert(ok == false)
  assert(tostring(err):match("tls%.version"))
end

local acme_auth_path = os.tmpname()
os.remove(acme_auth_path)
assert(vectis.auth.store_init({ credentials_path = acme_auth_path }))

local acme_missing_domain = assert(vectis.app.new({
  app_name = "lua-acme-missing-domain",
  port = 18180,
  tls = {
    mode = "acme",
    acme_email = "ops@example.com",
  },
}))
assert(acme_missing_domain:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
local acme_started, acme_error = acme_missing_domain:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("tls%.domains"))
acme_missing_domain:close()

local acme_duplicate, acme_duplicate_error = vectis.app.new({
  app_name = "lua-acme-duplicate-domain",
  port = 18181,
  tls = {
    mode = "acme",
    domains = { "api.example.com", "api.example.com" },
    email = "ops@example.com",
  },
})
assert(acme_duplicate == nil)
assert(type(acme_duplicate_error) == "table")
assert(acme_duplicate_error.status == vectis.ERR_INVALID)
assert(acme_duplicate_error.message:match("duplicate"))

local acme_missing_email = assert(vectis.app.new({
  app_name = "lua-acme-missing-email",
  port = 18182,
  tls = {
    mode = "acme",
    domains = { "api.example.com", "www.example.com" },
    provider = "https://acme.example.test/directory",
    cache_dir = "/tmp/vectis-lua-acme-cache",
  },
}))
assert(acme_missing_email:auth_json({
  path = "/probe",
  auth = { kind = "native", credentials_path = acme_auth_path },
}) == true)
acme_started, acme_error = acme_missing_email:start()
assert(acme_started == nil)
assert(type(acme_error) == "table")
assert(acme_error.status == vectis.ERR_INVALID)
assert(acme_error.message:match("acme_email"))
acme_missing_email:close()

assert(type(vectis.auth) == "table")
local function oauth_transport(mode)
  return function(request)
    assert(request.method == "POST")
    assert(request.url == "https://idp.example.test/token")
    assert(request.content_type == "application/x-www-form-urlencoded")
    assert(type(request.body) == "string")
    if mode == "code" then
      assert(request.body:find("grant_type=authorization_code", 1, true))
      assert(request.body:find("code=auth-code", 1, true))
      assert(request.body:find("client_id=vectis-client", 1, true))
      assert(request.body:find("code_verifier=", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = '{"access_token":"browser-token","token_type":"Bearer","refresh_token":"browser-refresh","scope":"openid dav","id_token":"id-token","expires_in":4200}',
      }
    end
    if mode == "client" then
      assert(request.body:find("grant_type=client_credentials", 1, true))
      assert(request.body:find("client_id=vectis-client", 1, true))
      assert(request.body:find("client_secret=vectis-secret", 1, true))
      return {
        status_code = 200,
        content_type = "application/json",
        body = '{"access_token":"m2m-token","token_type":"Bearer","refresh_token":"m2m-refresh","scope":"dav","expires_in":3600}',
      }
    end
    if mode == "fail" then
      assert(request.body:find("grant_type=refresh_token", 1, true))
      assert(request.body:find("refresh_token=lua-refresh-token", 1, true))
      error("mock OAuth2 refresh failed")
    end
    assert(request.body:find("grant_type=refresh_token", 1, true))
    assert(request.body:find("refresh_token=old-refresh", 1, true))
    return {
      status_code = 200,
      content_type = "application/json",
      body = '{"access_token":"refreshed-token","token_type":"Bearer","refresh_token":"new-refresh","scope":"dav","expires_in":7200}',
    }
  end
end
local oidc = assert(vectis.auth.oidc_authorization({
  authorization_endpoint = "https://idp.example.test/authorize",
  client_id = "vectis-client",
  redirect_uri = "http://127.0.0.1/callback",
  scope = "openid dav",
  state = "lua-state",
  nonce = "lua-nonce",
  code_verifier = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~abc",
}))
assert(oidc.authorization_url:find("https://idp.example.test/authorize?", 1, true) == 1)
assert(oidc.authorization_url:find("response_type=code", 1, true))
assert(oidc.authorization_url:find("code_challenge_method=S256", 1, true))
assert(oidc.code_verifier == "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~abc")
assert(type(oidc.code_challenge) == "string" and #oidc.code_challenge > 0)
assert(oidc.state == "lua-state")
assert(oidc.nonce == "lua-nonce")
local exchanged = assert(vectis.auth.oidc_exchange_callback({
  transport = oauth_transport("code"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  redirect_uri = "http://127.0.0.1/callback",
  code_verifier = oidc.code_verifier,
  callback_query = "?code=auth-code&state=lua-state",
  expected_state = "lua-state",
  now = 1000,
}))
assert(exchanged.code == "auth-code")
assert(exchanged.state == "lua-state")
assert(exchanged.token.access_token == "browser-token")
assert(exchanged.token.id_token == "id-token")
assert(exchanged.flow.access_token == "browser-token")
assert(exchanged.flow.expires_at == 5200)
local m2m = assert(vectis.auth.oauth2_client_credentials({
  transport = oauth_transport("client"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  scope = "dav",
}))
assert(m2m.access_token == "m2m-token")
assert(m2m.refresh_token == "m2m-refresh")
assert(m2m.expires_in == 3600)
local ensured = assert(vectis.auth.oauth2_flow_ensure({
  transport = oauth_transport("refresh"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  now = 1000,
  flow = {
    access_token = "old-token",
    token_type = "Bearer",
    refresh_token = "old-refresh",
    scope = "dav",
    expires_at = 900,
    has_expires_at = true,
  },
}))
assert(ensured.result.state == "refreshed")
assert(ensured.result.refreshed == true)
assert(ensured.flow.access_token == "refreshed-token")
assert(ensured.flow.refresh_token == "new-refresh")
assert(ensured.flow.expires_at == 8200)
local auth_path = os.tmpname()
os.remove(auth_path)
local auth_state_path = os.tmpname()
os.remove(auth_state_path)
local function file_contains(path, text)
  local file = io.open(path, "rb")
  if not file then
    return false
  end
  local body = file:read("*a")
  file:close()
  return body:find(text, 1, true) ~= nil
end
assert(vectis.auth.store_init({
  credentials_path = auth_path,
  state_path = auth_state_path,
}))
assert(vectis.auth.oauth2_flow_upsert({
  credentials_path = auth_path,
  flow_id = "lua-browser-flow",
  subject = "lua-browser-oidc@example.com",
  flow = exchanged.flow,
}))
local browser_flow_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-browser-flow",
  subject = "lua-browser-oidc@example.com",
}))
assert(type(browser_flow_key.client_id) == "string")
assert(type(browser_flow_key.client_secret) == "string")
assert(browser_flow_key.claim_json:match('"oauth2_flow_id":"lua%-browser%-flow"'))
local browser_flow_verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    browser_flow_key.client_id .. ":" .. browser_flow_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(browser_flow_verified.authenticated == true)
assert(browser_flow_verified.claim_json:match('"oauth2_flow_id":"lua%-browser%-flow"'))
assert(vectis.auth.oauth2_flow_upsert({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
  flow = {
    access_token = "lua-access-token",
    token_type = "Bearer",
    refresh_token = "lua-refresh-token",
    scope = "openid dav",
    id_token = "lua-id-token",
    expires_at = 5200,
    has_expires_at = true,
  },
}))
local loaded_flow = assert(vectis.auth.oauth2_flow_load({
  credentials_path = auth_path,
  flow_id = "lua-flow",
}))
assert(loaded_flow.found == true)
assert(loaded_flow.flow_id == "lua-flow")
assert(loaded_flow.subject == "lua-oidc@example.com")
assert(loaded_flow.flow.access_token == "lua-access-token")
assert(loaded_flow.flow.refresh_token == "lua-refresh-token")
assert(loaded_flow.flow.expires_at == 5200)
assert(loaded_flow.flow.has_expires_at == true)
local oauth_webdav_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
}))
assert(type(oauth_webdav_key.client_id) == "string")
assert(type(oauth_webdav_key.client_secret) == "string")
assert(oauth_webdav_key.claim_json:match('"oauth2_flow_id":"lua%-flow"'))
local oauth_webdav_verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_key.client_id .. ":" .. oauth_webdav_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_verified.authenticated == true)
assert(oauth_webdav_verified.claim_json:match('"oauth2_flow_id":"lua%-flow"'))
local stored_ensure, stored_ensure_error = vectis.auth.oauth2_stored_flow_ensure({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  transport = oauth_transport("fail"),
  token_endpoint = "https://idp.example.test/token",
  client_id = "vectis-client",
  client_secret = "vectis-secret",
  now = 6000,
})
assert(stored_ensure == nil)
assert(type(stored_ensure_error) == "table")
local oauth_webdav_revoked = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_key.client_id .. ":" .. oauth_webdav_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_revoked.authenticated == false)
local oauth_webdav_retained_key = assert(vectis.auth.oauth2_webdav_key({
  credentials_path = auth_path,
  flow_id = "lua-flow",
  subject = "lua-oidc@example.com",
}))
local retained_ensure, retained_ensure_error =
  vectis.auth.oauth2_stored_flow_ensure({
    credentials_path = auth_path,
    flow_id = "lua-flow",
    transport = oauth_transport("fail"),
    token_endpoint = "https://idp.example.test/token",
    client_id = "vectis-client",
    client_secret = "vectis-secret",
    now = 6000,
    revoke_webdav_keys_on_failure = false,
  })
assert(retained_ensure == nil)
assert(type(retained_ensure_error) == "table")
local oauth_webdav_retained = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Basic " .. base64_encode(
    oauth_webdav_retained_key.client_id .. ":" ..
      oauth_webdav_retained_key.client_secret),
  allowed_modes = { "basic" },
}))
assert(oauth_webdav_retained.authenticated == true)
local issued = assert(vectis.auth.issue({
  credentials_path = auth_path,
  subject = "lua@example.com",
  purpose = "webdav",
  modes = { "bearer" },
}))
assert(type(issued.client_id) == "string")
assert(type(issued.api_key) == "string")
assert(issued.client_secret == nil)
local verified = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Bearer " .. issued.api_key,
  allowed_modes = { "bearer" },
}))
assert(verified.authenticated == true)
assert(verified.auth_mode == "bearer")
assert(verified.claim_json:match('"purpose":"webdav"'))
local native_provider = assert(vectis.auth.provider_native({
  credentials_path = auth_path,
  purpose = "webdav",
  realm = "lua",
  allowed_modes = { "bearer" },
}))
local native_allowed = assert(native_provider:authenticate({
  authorization = "Bearer " .. issued.api_key,
}))
assert(native_allowed.action == "allow")
assert(native_allowed.principal == "lua@example.com")
local native_required = assert(native_provider:authenticate({}))
assert(native_required.action == "required")
assert(native_required.www_authenticate == "Bearer")
local callback_provider = assert(vectis.auth.provider_callback(function(request)
  return { action = "allow", principal = request.resource }
end))
local callback_allowed = assert(callback_provider:authenticate({ resource = "/lua" }))
assert(callback_allowed.action == "allow")
assert(callback_allowed.status_code == 0)
assert(callback_allowed.principal == "/lua")
local callback_required_provider = assert(vectis.auth.provider_callback(function(request)
  return {
    action = "required",
    status_code = 401,
    www_authenticate = 'Basic realm="' .. request.resource .. '"',
  }
end))
local callback_required = assert(callback_required_provider:authenticate({ resource = "lua" }))
assert(callback_required.action == "required")
assert(callback_required.status_code == 401)
assert(callback_required.www_authenticate == 'Basic realm="lua"')
local callback_redirect_provider = assert(vectis.auth.provider_callback(function()
  return {
    action = "redirect",
    status_code = 303,
    location = "/auth/login?next=/dav",
    content_type = "text/plain",
    body = "login required",
  }
end))
local callback_redirect = assert(callback_redirect_provider:authenticate({}))
assert(callback_redirect.action == "redirect")
assert(callback_redirect.status_code == 303)
assert(callback_redirect.location == "/auth/login?next=/dav")
assert(callback_redirect.content_type == "text/plain")
assert(callback_redirect.body == "login required")
local callback_deny_provider = assert(vectis.auth.provider_callback(function()
  return {}
end))
local callback_deny = assert(callback_deny_provider:authenticate({}))
assert(callback_deny.action == "deny")
local callback_invalid_provider = assert(vectis.auth.provider_callback(function()
  return { action = "maybe" }
end))
local callback_invalid, callback_invalid_error = callback_invalid_provider:authenticate({})
assert(callback_invalid == nil)
assert(callback_invalid_error.status_string == "invalid")
assert(callback_invalid_error.message:match("action"))
local user = assert(vectis.auth.user_add({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  email = "lua-user@example.com",
  totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ",
  totp_label = "Vectis:lua-user@example.com",
  issuer = "Vectis",
}))
assert(user.username == "lua-user@example.com")
assert(user.totp_secret == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
assert(user.totp_uri:find("otpauth://totp/", 1, true) == 1)
assert(user.totp_qr:find("\226\150\136", 1, true))
local missing_totp = assert(vectis.auth.user_login({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
}))
assert(missing_totp.authenticated == false)
local logged_in = assert(vectis.auth.user_login({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(logged_in.authenticated == true)
local email_token = assert(vectis.auth.email_token_issue({
  credentials_path = auth_path,
  state_path = auth_state_path,
  username = "lua-user@example.com",
  realm = "lua",
  email = "lua-user@example.com",
  pending_transaction_id = "lua-pending-1",
  transaction_id = "lua-email-tx-1",
  token = "123456",
  now = 1000,
  ttl_seconds = 300,
  max_attempts = 2,
}))
assert(file_contains(auth_state_path, "lua-email-tx-1"))
assert(not file_contains(auth_path, "lua-email-tx-1"))
assert(email_token.transaction_id == "lua-email-tx-1")
assert(email_token.token == "123456")
assert(email_token.expires_at == 1300)
local wrong_pending_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-other",
  token = "123456",
  now = 1100,
}))
assert(wrong_pending_email_token.verified == false)
assert(wrong_pending_email_token.expired == false)
assert(wrong_pending_email_token.failed_attempts == 0)
assert(wrong_pending_email_token.max_attempts == 0)
local wrong_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-1",
  token = "000000",
  now = 1100,
}))
assert(wrong_email_token.verified == false)
assert(wrong_email_token.expired == false)
assert(wrong_email_token.pending_transaction_id == "lua-pending-1")
assert(wrong_email_token.failed_attempts == 1)
assert(wrong_email_token.max_attempts == 2)
local verified_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  pending_transaction_id = "lua-pending-1",
  token = "123456",
  now = 1100,
}))
assert(verified_email_token.verified == true)
assert(verified_email_token.expired == false)
assert(verified_email_token.username == "lua-user@example.com")
assert(verified_email_token.realm == "lua")
assert(verified_email_token.email == "lua-user@example.com")
assert(verified_email_token.pending_transaction_id == "lua-pending-1")
assert(verified_email_token.failed_attempts == 1)
assert(verified_email_token.max_attempts == 2)
local replayed_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-1",
  username = "lua-user@example.com",
  realm = "lua",
  token = "123456",
  now = 1100,
}))
assert(replayed_email_token.verified == false)
assert(replayed_email_token.expired == false)
local expiring_email_token = assert(vectis.auth.email_token_issue({
  credentials_path = auth_path,
  state_path = auth_state_path,
  username = "lua-user@example.com",
  realm = "lua",
  email = "lua-user@example.com",
  transaction_id = "lua-email-tx-2",
  token = "654321",
  now = 1000,
  ttl_seconds = 300,
}))
assert(expiring_email_token.expires_at == 1300)
local expired_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-2",
  username = "lua-user@example.com",
  realm = "lua",
  token = "654321",
  now = 1400,
}))
assert(expired_email_token.verified == false)
assert(expired_email_token.expired == true)
local expired_replay_email_token = assert(vectis.auth.email_token_verify({
  credentials_path = auth_path,
  state_path = auth_state_path,
  transaction_id = "lua-email-tx-2",
  username = "lua-user@example.com",
  realm = "lua",
  token = "654321",
  now = 1400,
}))
assert(expired_replay_email_token.verified == false)
assert(expired_replay_email_token.expired == false)
local webdav_key = assert(vectis.auth.webdav_key({
  credentials_path = auth_path,
  username = "lua-user@example.com",
  password = "lua-password",
  totp_code = "287082",
  time = 59,
  window = 0,
}))
assert(type(webdav_key.client_id) == "string")
assert(type(webdav_key.client_secret) == "string")
local webdav_authorization = assert(vectis.auth.basic_authorization(webdav_key))
assert(webdav_authorization == "Basic " .. base64_encode(
  webdav_key.client_id .. ":" .. webdav_key.client_secret))
assert(vectis.auth.basic_authorization(
  webdav_key.client_id, webdav_key.client_secret) == webdav_authorization)
local missing_basic, missing_basic_err =
  vectis.auth.basic_authorization({client_id = webdav_key.client_id})
assert(missing_basic == nil)
assert(missing_basic_err.status == vectis.ERR_INVALID)
assert(missing_basic_err.message:find("client_secret", 1, true))
local totp = assert(vectis.auth.totp.new("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"))
assert(totp:secret() == "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
assert(totp:generate(59) == "287082")
assert(totp:validate("287082", 59, 0))
assert(not totp:validate("287083", 59, 0))
assert(totp:uri("Vectis:auth", "Vectis") ==
  "otpauth://totp/Vectis%3Aauth?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Vectis")
assert(totp:qr("Vectis:auth", "Vectis"):find("\226\150\136", 1, true))
local qr = assert(vectis.auth.qr.new("vectis"))
assert(qr:size() > 0)
assert(qr:ansi():find("\226\150\136", 1, true))
assert(vectis.auth.revoke({ credentials_path = auth_path, client_id = issued.client_id }))
local revoked = assert(vectis.auth.verify({
  credentials_path = auth_path,
  authorization = "Bearer " .. issued.api_key,
  allowed_modes = { "bearer" },
}))
assert(revoked.authenticated == false)
os.remove(auth_path)
os.remove(auth_path .. ".lock")

assert(type(lonejson) == "table")
assert(lonejson.encode_json(lonejson.json_null) == "null")

assert(type(lockdc) == "table")
assert(type(lockdc.open) == "function")
assert(type(lockdc.version_string()) == "string")
assert(lockdc.encode_json({ ok = true }) == '{"ok":true}')

assert(type(lql) == "table")
assert(type(lql.core) == "table")
assert(lql.core == require("lql.core"))
assert(type(lql.version()) == "string")
assert(type(lql.new) == "function")

assert(type(cai) == "table")
assert(type(cai.open) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.mcp_client) == "function")
assert(type(cai.MODEL_DEFAULT_RESPONSES) == "string")
assert(type(cai.MCP_PROTOCOL_VERSION) == "string")

assert(type(curl) == "table")
assert(type(curl.perform) == "function")
assert(type(curl.json) == "function")
assert(type(curl.stream_json) == "function")
assert(type(curl.version()) == "string")

assert(type(zlib) == "table")
assert(type(zlib.version()) == "string")
local zlib_payload = ("vectis zlib payload\n"):rep(32)
local deflated = assert(zlib.deflate(zlib_payload, { level = 9 }))
assert(deflated ~= zlib_payload)
assert(assert(zlib.inflate(deflated)) == zlib_payload)
assert(assert(zlib.decompress(deflated)) == zlib_payload)
assert(assert(zlib.decompress(assert(zlib.compress(zlib_payload)))) == zlib_payload)
local gzipped = assert(zlib.gzip(zlib_payload))
assert(assert(zlib.gunzip(gzipped)) == zlib_payload)
assert(assert(zlib.decompress(gzipped)) == zlib_payload)
local limited, limited_err = zlib.inflate(deflated, { max_output_bytes = 8 })
assert(limited == nil)
assert(limited_err.status == vectis.ERR_INVALID)
assert(limited_err.message:find("max_output_bytes", 1, true))
local invalid_zlib, invalid_zlib_err = zlib.decompress("not compressed")
assert(invalid_zlib == nil)
assert(invalid_zlib_err.status == vectis.ERR_INVALID)
local zlib_file_in = os.tmpname()
local zlib_file_gz = os.tmpname()
local zlib_file_out = os.tmpname()
local zlib_file_limited = os.tmpname()
local zlib_file_fp = assert(io.open(zlib_file_in, "wb"))
zlib_file_fp:write(zlib_payload)
zlib_file_fp:close()
local gzip_file_result = assert(zlib.gzip_file({
  input_path = zlib_file_in,
  output_path = zlib_file_gz,
  level = 9,
}))
assert(gzip_file_result.ok == true)
assert(gzip_file_result.input_bytes == #zlib_payload)
assert(gzip_file_result.output_bytes > 0)
local gunzip_file_result = assert(zlib.gunzip_file({
  input_path = zlib_file_gz,
  output_path = zlib_file_out,
}))
assert(gunzip_file_result.ok == true)
assert(gunzip_file_result.output_bytes == #zlib_payload)
zlib_file_fp = assert(io.open(zlib_file_out, "rb"))
assert(zlib_file_fp:read("*a") == zlib_payload)
zlib_file_fp:close()
local limited_file, limited_file_err = zlib.decompress_file({
  input_path = zlib_file_gz,
  output_path = zlib_file_limited,
  max_output_bytes = 8,
})
assert(limited_file == nil)
assert(limited_file_err.status == vectis.ERR_INVALID)
assert(limited_file_err.message:find("max_output_bytes", 1, true))
os.remove(zlib_file_in)
os.remove(zlib_file_gz)
os.remove(zlib_file_out)
os.remove(zlib_file_limited)

assert(type(vectis.http) == "table")
assert(type(vectis.http.get) == "function")
assert(type(vectis.http.post) == "function")
assert(type(vectis.http.put) == "function")
assert(type(vectis.http.patch) == "function")
assert(type(vectis.http.delete) == "function")
assert(type(vectis.http.form) == "function")
assert(type(vectis.http.form_encode) == "function")
assert(type(vectis.http.multipart) == "function")
assert(type(vectis.http.client) == "function")
assert(type(vectis.http.request_json) == "function")
assert(type(vectis.http.download) == "function")

assert(type(vectis.lockd) == "table")
assert(type(vectis.lockd.config) == "function")
assert(type(vectis.lockd.open) == "function")
assert(type(vectis.lockd.with_client) == "function")
assert(type(vectis.lockd.enqueue_json) == "function")
assert(type(vectis.lockd.load_json) == "function")
assert(type(vectis.lockd.save_json) == "function")
assert(type(vectis.lockd.with_acquired_lease) == "function")
assert(vectis.lockd.native == lockdc)
assert(vectis.lockd.encode_json({ ok = true }) == '{"ok":true}')
assert(vectis.lockd.decode_json('{"ok":true}').ok == true)
assert(vectis.lockd.json_null == lockdc.json_null)
assert(lockd == vectis.lockd)
local normalized_lockd = assert(vectis.lockd.config({
  endpoints = { "https://127.0.0.1:1" },
  namespace = "lua-lockd",
  client_bundle = "/tmp/vectis-lockd-client.pem",
}))
assert(normalized_lockd.default_namespace == "lua-lockd")
assert(normalized_lockd.namespace == nil)
assert(normalized_lockd.client_bundle == nil)
assert(normalized_lockd.client_bundle_path == "/tmp/vectis-lockd-client.pem")
local embedded_lockd_config, embedded_lockd_err =
    vectis.lockd.config({ client_bundle = "embedded" })
assert(embedded_lockd_config == nil)
assert(type(embedded_lockd_err) == "table")
assert(embedded_lockd_err.status == vectis.ERR_STATE)
assert(embedded_lockd_err.status_string == "state")
assert(embedded_lockd_err.source == "vectis")
assert(embedded_lockd_err.source_code == vectis.ERROR_SOURCE_VECTIS)
assert(embedded_lockd_err.message == "no embedded lockd bundle")

assert(type(vectis.cert) == "table")
assert(type(vectis.cert.generate_bundle) == "function")
assert(type(vectis.cert.generate_private_key) == "function")
assert(type(vectis.cert.generate_csr) == "function")
assert(type(vectis.cert.inspect_bundle) == "function")
assert(type(vectis.cert.validate_bundle) == "function")
assert(type(vectis.cert.validate_pair) == "function")
assert(type(vectis.auth.basic_authorization) == "function")

assert(type(pslog) == "table")
assert(type(pslog.new_json) == "function")
assert(type(pslog.version()) == "string")
local log_chunks = {}
local raw_log = assert(pslog.new_json(function(chunk)
  log_chunks[#log_chunks + 1] = chunk
end, { timestamps = false }))
raw_log:info("lua smoke", "component", "vectis")
raw_log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:match('"msg":"lua smoke"'))
assert(log_payload:match('"component":"vectis"'))
assert(log.native == pslog)
local vectis_log_chunks = {}
local vectis_logger = assert(log.new({
  output = function(chunk)
    vectis_log_chunks[#vectis_log_chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
  fields = { service = "smoke" },
}))
local _, log_status_err = nil, status.error({
  kind = "smoke",
  message = "expected",
  status = status.ERR_INVALID,
  source_code = status.ERROR_SOURCE_VECTIS,
})
assert(log.log_error(vectis_logger, "error", "structured smoke",
                     log_status_err, { path = "/smoke" }))
vectis_logger:close()
local vectis_log_payload = table.concat(vectis_log_chunks)
assert(vectis_log_payload:match('"service":"smoke"'))
assert(vectis_log_payload:match('"status_string":"invalid"'))
assert(vectis_log_payload:match('"source":"vectis"'))
assert(vectis_log_payload:match('"path":"/smoke"'))
local level_ok, level_err =
    log.log_error(vectis_logger, "verbose", "bad level", log_status_err)
assert(level_ok == nil)
assert(level_err.status == vectis.ERR_INVALID)
assert(level_err.source_code == vectis.ERROR_SOURCE_VECTIS)

assert(type(libmdf) == "table")
assert(type(libmdf.render) == "function")
assert(type(libmdf.render_stream) == "function")
assert(libmdf.version == "0.8.0")
local rendered_markdown = libmdf.render("# Vectis\n\n**ok**", { format = "html" })
assert(rendered_markdown:match("Vectis"))
assert(rendered_markdown:match("ok"))

assert(type(softline) == "table")
assert(type(softline.new) == "function")
local line = assert(softline.new({ line_max_len = 32 }))
assert(line:set_buffer("draft"))
assert(line:insert("++"))
assert(line:buffer():match("%+%+"))
line:close()

local encoded = lonejson.encode_json({
  b = true,
  a = lonejson.json_array({ "first", lonejson.json_null, 3 }),
})
assert(encoded == '{"a":["first",null,3],"b":true}')

local decoded = lonejson.decode_json(encoded)
assert(decoded.a[1] == "first")
assert(decoded.a[2] == lonejson.json_null)
assert(decoded.a[3] == 3)
assert(decoded.b == true)

local chunks = {}
lonejson.encode_json_to_sink({ z = "sink", a = lonejson.json_array({ true, false }) }, function(chunk)
  chunks[#chunks + 1] = chunk
end)
assert(table.concat(chunks) == '{"a":[true,false],"z":"sink"}')
