local curl = require("curl")

local work_dir = os.getenv("VECTIS_LUA_CURL_PROTOCOL_EXAMPLE_DIR") or "."
local source_path = work_dir .. "/vectis-lua-curl-source.txt"
local download_path = work_dir .. "/vectis-lua-curl-download.txt"
local upload_path = work_dir .. "/vectis-lua-curl-upload.txt"

os.remove(source_path)
os.remove(download_path)
os.remove(upload_path)
local source_file = assert(io.open(source_path, "wb"))
source_file:write("generic curl protocol payload\n")
source_file:close()

local download = curl.perform({
  url = "file://" .. source_path,
  protocols = "file",
  download_path = download_path,
  timeout_ms = 1000,
  no_signal = true,
})
assert(download.ok == true, download.error)
assert(download.body == "")
local downloaded_file = assert(io.open(download_path, "rb"))
assert(downloaded_file:read("*a") == "generic curl protocol payload\n")
downloaded_file:close()

local upload = curl.perform({
  url = "file://" .. upload_path,
  protocols = "file",
  upload = true,
  body_path = source_path,
  timeout_ms = 1000,
  no_signal = true,
})
assert(upload.ok == true, upload.error)
local uploaded_file = assert(io.open(upload_path, "rb"))
assert(uploaded_file:read("*a") == "generic curl protocol payload\n")
uploaded_file:close()

local blocked = curl.perform({
  url = "file://" .. source_path,
  protocols = "http,https",
  timeout_ms = 1000,
  no_signal = true,
})
assert(blocked.ok == false)
assert(type(blocked.error) == "string")

os.remove(source_path)
os.remove(download_path)
os.remove(upload_path)

print("lua curl protocols example ok")
