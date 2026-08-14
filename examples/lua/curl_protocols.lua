local curl = require("curl")

local function join_path(dir, name)
  if dir:sub(-1) == "/" then
    return dir .. name
  end
  return dir .. "/" .. name
end

local function read_file(path)
  local fp = assert(io.open(path, "rb"))
  local body = fp:read("*a")
  fp:close()
  return body
end

local function write_file(path, body)
  local fp = assert(io.open(path, "wb"))
  fp:write(body)
  fp:close()
end

local work_dir = os.getenv("VECTIS_LUA_CURL_PROTOCOL_EXAMPLE_DIR") or "."
local source_path = join_path(work_dir, "vectis-lua-curl-source.txt")
local download_path = join_path(work_dir, "vectis-lua-curl-download.txt")
local upload_path = join_path(work_dir, "vectis-lua-curl-upload.txt")

os.remove(source_path)
os.remove(download_path)
os.remove(upload_path)
write_file(source_path, "generic curl protocol payload\n")

local download = curl.perform({
  url = "file://" .. source_path,
  protocols = "file",
  download_path = download_path,
  timeout_ms = 1000,
  no_signal = true,
})
assert(download.ok == true, download.error)
assert(download.body == "")
assert(read_file(download_path) == "generic curl protocol payload\n")

local upload = curl.perform({
  url = "file://" .. upload_path,
  protocols = "file",
  upload = true,
  body_path = source_path,
  timeout_ms = 1000,
  no_signal = true,
})
assert(upload.ok == true, upload.error)
assert(read_file(upload_path) == "generic curl protocol payload\n")

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
