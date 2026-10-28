#include <cpkt/lz4.h>
#include <cpkt/lzma.h>
#include <cpkt/zstd.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/compression.h>

typedef struct compression_state {
  vectis_compression receiver;
  int algorithm, compress, busy, violation, started, ended, finished;
  pid_t process;
  pthread_t thread;
  vectis_error failure;
  cpkt_zstd_ZSTD_CCtx *zc;
  cpkt_zstd_ZSTD_DCtx *zd;
  cpkt_lz4_LZ4F_cctx *lc;
  cpkt_lz4_LZ4F_dctx *ld;
  cpkt_lz4_LZ4F_preferences_t prefs;
  cpkt_lzma_lzma_stream xz;
  unsigned char output[VECTIS_COMPRESSION_CHUNK_BYTES];
} compression_state;

static vectis_status compression_error(vectis_error *error, vectis_status code,
                                       long native, const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    error->dependency_code = native;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return code;
}
void vectis_compression_config_init(vectis_compression_config *config) {
  if (config != NULL) {
    config->level = 0;
    config->level_set = 0;
    config->max_window_log = 26u;
    config->memory_limit = 67108864u;
  }
}
static vectis_status compression_check(compression_state *state,
                                       vectis_error *error) {
  if (state == NULL)
    return compression_error(error, VECTIS_ERR_INVALID, 0,
                             "missing compression receiver");
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return compression_error(
        error, VECTIS_ERR_STATE, 0,
        "compression receiver belongs to another process/thread");
  if (state->busy) {
    state->violation = 1;
    return compression_error(
        error, VECTIS_ERR_STATE, 0,
        "compression callback cannot reenter or close its receiver");
  }
  if (state->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = state->failure;
    return state->failure.code;
  }
  return VECTIS_OK;
}
static void compression_release(compression_state *state) {
  if (state->zc)
    cpkt_zstd_ZSTD_freeCCtx(state->zc);
  if (state->zd)
    cpkt_zstd_ZSTD_freeDCtx(state->zd);
  if (state->lc)
    cpkt_lz4_LZ4F_freeCompressionContext(state->lc);
  if (state->ld)
    cpkt_lz4_LZ4F_freeDecompressionContext(state->ld);
  if (state->algorithm == 3)
    cpkt_lzma_lzma_end(&state->xz);
  free(state);
}
vectis_status vectis_compression_close(vectis_compression *receiver,
                                       vectis_error *error) {
  compression_state *state;
  if (receiver == NULL)
    return VECTIS_OK;
  state = (compression_state *)receiver->impl;
  /* Closing inherited state discards the child's copy without executing any
   * user callback; operations on it remain forbidden. */
  if (state->busy) {
    state->violation = 1;
    return compression_error(error, VECTIS_ERR_STATE, 0,
                             "cannot close active compression receiver");
  }
  if (state->process == getpid() &&
      !pthread_equal(state->thread, pthread_self()))
    return compression_error(error, VECTIS_ERR_STATE, 0,
                             "compression close on foreign thread");
  compression_release(state);
  return VECTIS_OK;
}
static vectis_status compression_zerror(size_t result, vectis_error *error) {
  if (!cpkt_zstd_ZSTD_isError(result))
    return VECTIS_OK;
  return compression_error(error, VECTIS_ERR_STATE,
                           (long)cpkt_zstd_ZSTD_getErrorCode(result),
                           cpkt_zstd_ZSTD_getErrorName(result));
}
static vectis_status compression_lerror(size_t result, vectis_error *error) {
  if (!cpkt_lz4_LZ4F_isError(result))
    return VECTIS_OK;
  return compression_error(error, VECTIS_ERR_STATE,
                           (long)cpkt_lz4_LZ4F_getErrorCode(result),
                           cpkt_lz4_LZ4F_getErrorName(result));
}
vectis_status vectis_compression_new(const char *algorithm,
                                     const char *direction,
                                     const vectis_compression_config *config,
                                     vectis_compression **out,
                                     vectis_error *error) {
  vectis_compression_config defaults;
  compression_state *state;
  vectis_status status = VECTIS_OK;
  size_t result;
  int which, compress;
  cpkt_compression_u64 memory;
  cpkt_lzma_lzma_ret xresult;
  if (out != NULL)
    *out = NULL;
  vectis_compression_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  which = algorithm == NULL                ? 0
          : strcmp(algorithm, "zstd") == 0 ? 1
          : strcmp(algorithm, "lz4") == 0  ? 2
          : strcmp(algorithm, "xz") == 0   ? 3
                                           : 0;
  compress = direction != NULL && strcmp(direction, "compress") == 0;
  if (out == NULL || which == 0 || direction == NULL ||
      (!compress && strcmp(direction, "decompress") != 0))
    return compression_error(error, VECTIS_ERR_INVALID, 0,
                             "expected zstd/lz4/xz and compress/decompress");
  if ((config->level_set != 0 && config->level_set != 1) ||
      (!compress && config->level_set) ||
      ((which != 1 || compress) &&
       config->max_window_log != defaults.max_window_log) ||
      ((which != 3 || compress) &&
       config->memory_limit != defaults.memory_limit) ||
      (which == 1 &&
       (config->max_window_log < 10u || config->max_window_log > 31u)) ||
      (which == 3 && config->memory_limit == 0u) ||
      (compress && config->level_set &&
       ((which == 1 && (config->level < cpkt_zstd_ZSTD_minCLevel() ||
                        config->level > cpkt_zstd_ZSTD_maxCLevel())) ||
        (which == 2 &&
         (config->level < 0 ||
          config->level > cpkt_lz4_LZ4F_compressionLevel_max())) ||
        (which == 3 && (config->level < 0 || config->level > 9)))))
    return compression_error(error, VECTIS_ERR_INVALID, 0,
                             "invalid or inapplicable compression option");
  state = (compression_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return compression_error(error, VECTIS_ERR_NOMEM, 0,
                             "allocating compression receiver");
  state->algorithm = which;
  state->compress = compress;
  state->process = getpid();
  state->thread = pthread_self();
  if (which == 1) {
    if (compress) {
      state->zc = cpkt_zstd_ZSTD_createCCtx();
      if (state->zc == NULL)
        status = compression_error(error, VECTIS_ERR_NOMEM, 0,
                                   "allocating Zstandard encoder");
      else
        status =
            compression_zerror(cpkt_zstd_ZSTD_CCtx_setParameter(
                                   state->zc, CPKT_ZSTD_ZSTD_c_compressionLevel,
                                   config->level_set ? config->level : 0),
                               error);
    } else {
      state->zd = cpkt_zstd_ZSTD_createDCtx();
      if (state->zd == NULL)
        status = compression_error(error, VECTIS_ERR_NOMEM, 0,
                                   "allocating Zstandard decoder");
      else
        status =
            compression_zerror(cpkt_zstd_ZSTD_DCtx_setParameter(
                                   state->zd, CPKT_ZSTD_ZSTD_d_windowLogMax,
                                   (int)config->max_window_log),
                               error);
    }
  } else if (which == 2) {
    if (compress)
      result = cpkt_lz4_LZ4F_createCompressionContext(&state->lc,
                                                      CPKT_LZ4_LZ4F_VERSION);
    else
      result = cpkt_lz4_LZ4F_createDecompressionContext(&state->ld,
                                                        CPKT_LZ4_LZ4F_VERSION);
    status = compression_lerror(result, error);
    state->prefs.frameInfo.blockSizeID = CPKT_LZ4_LZ4F_max64KB;
    state->prefs.frameInfo.contentChecksumFlag =
        CPKT_LZ4_LZ4F_contentChecksumEnabled;
    state->prefs.compressionLevel = config->level_set ? config->level : 0;
    state->prefs.autoFlush = 1u;
  } else {
    memory.lo = (unsigned int)config->memory_limit;
    memory.hi = (unsigned int)((config->memory_limit / 65536u) / 65536u);
    if (compress)
      xresult = cpkt_lzma_lzma_easy_encoder(
          &state->xz, config->level_set ? (unsigned int)config->level : 6u,
          CPKT_LZMA_LZMA_CHECK_CRC64);
    else
      xresult = cpkt_lzma_lzma_stream_decoder(&state->xz, memory, 0u);
    if (xresult != CPKT_LZMA_LZMA_OK)
      status = compression_error(error, VECTIS_ERR_STATE, (long)xresult,
                                 "initializing XZ codec");
  }
  if (status != VECTIS_OK) {
    compression_release(state);
    return status;
  }
  state->receiver.process = vectis_compression_process;
  state->receiver.finish = vectis_compression_finish;
  state->receiver.close = vectis_compression_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
static vectis_status compression_emit(compression_state *state, size_t size,
                                      vectis_compression_sink sink,
                                      void *context, vectis_error *error) {
  vectis_status status;
  if (size == 0u)
    return VECTIS_OK;
  status = sink(context, state->output, size, error);
  if (state->violation)
    return compression_error(
        error, VECTIS_ERR_STATE, 0,
        "compression callback attempted receiver reentry/close");
  if (status != VECTIS_OK && error->code == VECTIS_OK)
    compression_error(error, status, 0, "compression sink failed");
  return status;
}
/* Each provider call returns before a sink is invoked. Input is sliced, output
 * is bounded, and nothing accumulates in proportion to total payload size. */
static vectis_status compression_run(compression_state *state, const void *data,
                                     size_t size, int finish,
                                     vectis_compression_sink sink,
                                     void *context, vectis_error *error) {
  const unsigned char *input = (const unsigned char *)data;
  size_t remaining = size, take, used, produced, result;
  int more = 1;
  vectis_status status;
  cpkt_zstd_ZSTD_inBuffer zin;
  cpkt_zstd_ZSTD_outBuffer zout;
  cpkt_lzma_lzma_ret xresult;
  if (state->algorithm == 2 && state->compress && !state->started) {
    result = cpkt_lz4_LZ4F_compressBegin(state->lc, state->output,
                                         sizeof(state->output), &state->prefs);
    status = compression_lerror(result, error);
    if (status != VECTIS_OK)
      return status;
    state->started = 1;
    status = compression_emit(state, result, sink, context, error);
    if (status != VECTIS_OK)
      return status;
  }
  while (more) {
    take = remaining > 32768u ? 32768u : remaining;
    used = produced = 0u;
    if (state->algorithm == 1) {
      zin.src = input;
      zin.size = take;
      zin.pos = 0u;
      zout.dst = state->output;
      zout.size = sizeof(state->output);
      zout.pos = 0u;
      result =
          state->compress
              ? cpkt_zstd_ZSTD_compressStream2(
                    state->zc, &zout, &zin,
                    finish ? CPKT_ZSTD_ZSTD_e_end : CPKT_ZSTD_ZSTD_e_continue)
              : cpkt_zstd_ZSTD_decompressStream(state->zd, &zout, &zin);
      status = compression_zerror(result, error);
      if (status != VECTIS_OK)
        return status;
      used = zin.pos;
      produced = zout.pos;
      if (result == 0u && (!state->compress || finish))
        state->ended = 1;
    } else if (state->algorithm == 2) {
      if (state->compress) {
        result = finish ? cpkt_lz4_LZ4F_compressEnd(state->lc, state->output,
                                                    sizeof(state->output), NULL)
                        : cpkt_lz4_LZ4F_compressUpdate(state->lc, state->output,
                                                       sizeof(state->output),
                                                       input, take, NULL);
        used = take;
        status = compression_lerror(result, error);
        if (status != VECTIS_OK)
          return status;
        produced = result;
        if (finish)
          state->ended = 1;
      } else {
        used = take;
        produced = sizeof(state->output);
        result = cpkt_lz4_LZ4F_decompress(state->ld, state->output, &produced,
                                          input, &used, NULL);
        status = compression_lerror(result, error);
        if (status != VECTIS_OK)
          return status;
        if (result == 0u)
          state->ended = 1;
      }
    } else {
      state->xz.next_in = input;
      state->xz.avail_in = take;
      state->xz.next_out = state->output;
      state->xz.avail_out = sizeof(state->output);
      xresult = cpkt_lzma_lzma_code(&state->xz, finish ? CPKT_LZMA_LZMA_FINISH
                                                       : CPKT_LZMA_LZMA_RUN);
      used = take - state->xz.avail_in;
      produced = sizeof(state->output) - state->xz.avail_out;
      /* Caller memory is never retained across calls or callbacks. */
      state->xz.next_in = NULL;
      state->xz.next_out = NULL;
      state->xz.avail_in = state->xz.avail_out = 0u;
      if (xresult != CPKT_LZMA_LZMA_OK && xresult != CPKT_LZMA_LZMA_STREAM_END)
        return compression_error(error, VECTIS_ERR_STATE, (long)xresult,
                                 "XZ codec failed");
      if (xresult == CPKT_LZMA_LZMA_STREAM_END)
        state->ended = 1;
    }
    if (used != 0u)
      input += used;
    remaining -= used;
    if (state->ended && remaining != 0u)
      return compression_error(error, VECTIS_ERR_INVALID, 0,
                               "trailing bytes after compression frame");
    status = compression_emit(state, produced, sink, context, error);
    if (status != VECTIS_OK)
      return status;
    if (state->ended)
      break;
    if (used == 0u && produced == 0u) {
      if (finish || remaining != 0u)
        return compression_error(error, VECTIS_ERR_STATE, 0,
                                 finish ? "truncated compression frame"
                                        : "codec made no progress");
      break;
    }
    more = remaining != 0u || finish || produced == sizeof(state->output);
  }
  if (finish && !state->ended)
    return compression_error(error, VECTIS_ERR_STATE, 0,
                             "truncated compression frame");
  return VECTIS_OK;
}
static vectis_status compression_call(vectis_compression *receiver,
                                      const void *data, size_t size, int finish,
                                      vectis_compression_sink sink,
                                      void *context, vectis_error *error) {
  compression_state *state =
      receiver == NULL ? NULL : (compression_state *)receiver->impl;
  vectis_error local;
  vectis_status status = compression_check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (sink == NULL || (data == NULL && size != 0u))
    return compression_error(error, VECTIS_ERR_INVALID, 0,
                             "missing compression input/sink");
  if (finish && state->finished)
    return VECTIS_OK;
  if (!finish && state->finished)
    return compression_error(error, VECTIS_ERR_STATE, 0,
                             "compression receiver is finished");
  if (!finish && size == 0u)
    return VECTIS_OK;
  memset(&local, 0, sizeof(local));
  if (state->ended) {
    if (size != 0u)
      status = compression_error(&local, VECTIS_ERR_INVALID, 0,
                                 "trailing bytes after compression frame");
  } else {
    state->busy = 1;
    status = compression_run(state, data, size, finish, sink, context, &local);
    state->busy = 0;
  }
  if (status != VECTIS_OK) {
    state->failure = local;
    if (error != NULL)
      *error = local;
  } else if (finish)
    state->finished = 1;
  return status;
}
vectis_status vectis_compression_process(vectis_compression *receiver,
                                         const void *data, size_t size,
                                         vectis_compression_sink sink,
                                         void *context, vectis_error *error) {
  return compression_call(receiver, data, size, 0, sink, context, error);
}
vectis_status vectis_compression_finish(vectis_compression *receiver,
                                        vectis_compression_sink sink,
                                        void *context, vectis_error *error) {
  return compression_call(receiver, NULL, 0u, 1, sink, context, error);
}
