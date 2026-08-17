local M = {
  runtime_available = false,
  runtime_model = "external",
  MAX_WORKER_COUNT = 253,
  DEFAULT_WEBSOCKET_MAX_FRAME_BYTES = 16384,
  DEFAULT_WEBSOCKET_TIMEOUT_MS = 120000,
  WORKER_DEATH_RESTART = 0,
  WORKER_DEATH_TERMINATE = 1,
  websocket = {
    CONTINUATION = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0a,
  },
}

return M
