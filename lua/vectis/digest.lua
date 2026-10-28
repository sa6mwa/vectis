local core = require("vectis.digest.core")

-- Public libvectis receiver and convenience operations, using the embedded
-- binding that calls only vectis/digest.h. No whole-input staging here.
return {
  new = core.digest_stream_new,
  materialized = core.digest_materialized,
  materialized_hex = core.digest_materialized_hex,
  stream = core.digest_stream,
  stream_hex = core.digest_stream_hex,
  file = core.digest_file,
  file_hex = core.digest_file_hex,
  hmac_new = core.hmac_stream_new,
  hmac_materialized = core.hmac_materialized,
  hmac_materialized_hex = core.hmac_materialized_hex,
  hmac_stream = core.hmac_stream,
  hmac_stream_hex = core.hmac_stream_hex,
  hmac_file = core.hmac_file,
  hmac_file_hex = core.hmac_file_hex,
  MAX_DIGEST_BYTES = core.DIGEST_MAX_BYTES,
  CHUNK_BYTES = core.DIGEST_CHUNK_BYTES,
}
