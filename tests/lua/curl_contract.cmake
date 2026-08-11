set(cli_file "${VECTIS_SOURCE_DIR}/src/vectis_cli.c")
set(curl_lua_file "${VECTIS_SOURCE_DIR}/lua/curl/init.lua")
set(curl_doc "${VECTIS_SOURCE_DIR}/docs/lua-curl.md")
set(readme_file "${VECTIS_SOURCE_DIR}/README.md")

file(READ "${cli_file}" cli_text)
file(READ "${curl_lua_file}" curl_lua_text)
file(READ "${curl_doc}" curl_doc_text)
file(READ "${readme_file}" readme_text)

foreach(required_text IN ITEMS
    "vectis_curl_lua_init.h"
    "vectis_lua_curl_perform"
    "vectis_lua_curl_apply_headers"
    "vectis_lua_curl_apply_method"
    "vectis_lua_curl_apply_smtp"
    "vectis_lua_curl_apply_tls"
    "vectis_lua_curl_apply_upload"
    "vectis_lua_curl_apply_protocol_options"
    "CURLOPT_PROTOCOLS_STR"
    "CURLOPT_USERNAME"
    "CURLOPT_PASSWORD"
    "CURLOPT_HTTPHEADER"
    "CURLOPT_CUSTOMREQUEST"
    "PROPFIND"
    "MKCOL"
    "COPY"
    "MOVE"
    "CURLOPT_POSTFIELDS"
    "CURLOPT_HTTP_VERSION"
    "CURL_HTTP_VERSION_2TLS"
    "CURLOPT_PROXY"
    "CURLOPT_PROXYTYPE"
    "CURLOPT_PROXYUSERNAME"
    "CURLOPT_PROXYPASSWORD"
    "CURLOPT_INTERFACE"
    "CURLOPT_USERAGENT"
    "CURLOPT_ACCEPT_ENCODING"
    "CURLOPT_UPLOAD"
    "CURLOPT_SSH_PRIVATE_KEYFILE"
    "CURLOPT_SSH_PUBLIC_KEYFILE"
    "CURLOPT_SSH_KNOWNHOSTS"
    "CURLOPT_CAPATH"
    "CURLOPT_SSLCERT"
    "CURLOPT_SSLKEY"
    "CURLOPT_KEYPASSWD"
    "CURLOPT_TCP_KEEPALIVE"
    "CURLOPT_NOSIGNAL"
    "CURLOPT_MAIL_FROM"
    "CURLOPT_MAIL_RCPT"
    "CURLOPT_USE_SSL"
    "CURLOPT_CAINFO"
    "CURLOPT_SSL_VERIFYPEER"
    "CURLOPT_SSL_VERIFYHOST"
    "VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT"
    "VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT"
    "vectis_lua_curl_parse_retry_config"
    "vectis_lua_curl_retry_status"
    "vectis_lua_curl_retry_code"
    "VECTIS_HTTP_RETRY_TRANSPORT"
    "VECTIS_HTTP_RETRY_429"
    "VECTIS_HTTP_RETRY_5XX"
    "vectis_lua_curl_stream_response_write"
    "lonejson_curl_read_callback"
    "lonejson_curl_write_callback"
    "curl_global_init")
  string(FIND "${cli_text}" "${required_text}" required_text_index)
  if(required_text_index EQUAL -1)
    message(FATAL_ERROR "Lua curl implementation is missing ${required_text}")
  endif()
endforeach()

foreach(required_text IN ITEMS
    "local core = require(\"curl.core\")"
    "local lonejson = require(\"lonejson\")"
    "function M.perform(opts)"
    "function M.json(opts)"
    "function M.stream_json(opts)"
    "lonejson.encode_value"
    "lonejson.decode_value"
    "opts.request_schema = opts.request.schema"
    "opts.response_schema = opts.response.schema")
  string(FIND "${curl_lua_text}" "${required_text}" required_text_index)
  if(required_text_index EQUAL -1)
    message(FATAL_ERROR "Lua curl facade is missing ${required_text}")
  endif()
endforeach()

foreach(required_text IN ITEMS
    "preloads `curl`"
    "curl.perform(opts)"
    "curl.json(opts)"
    "curl.stream_json(opts)"
    "SMTP"
    "SFTP/SCP"
    "FTP/file"
    "HTTP/HTTPS"
    "WebDAV"
    "MQTT"
    "`protocols`"
    "`smtp`"
    "`ssh_private_key`, `ssh_public_key`, `ssh_known_hosts`"
    "`upload`"
    "`retry`"
    "`retry_max_attempts`, `retry_initial_delay_ms`, `retry_max_delay_ms`"
    "lonejson_curl_read_callback()"
    "lonejson_curl_write_callback()")
  string(FIND "${curl_doc_text}" "${required_text}" required_text_index)
  if(required_text_index EQUAL -1)
    message(FATAL_ERROR "Lua curl documentation is missing ${required_text}")
  endif()
endforeach()

foreach(required_text IN ITEMS
    "curl.perform()"
    "curl.json()"
    "curl.stream_json()")
  string(FIND "${readme_text}" "${required_text}" required_text_index)
  if(required_text_index EQUAL -1)
    message(FATAL_ERROR "README is missing Lua curl summary ${required_text}")
  endif()
endforeach()
