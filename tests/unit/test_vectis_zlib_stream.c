/* Private implementation inclusion is exclusively for tiny counter faults. */
#include "../../src/vectis_zlib.c"
#include "vectis_lua_zlib_stream.h"
#include <assert.h>
#include <lauxlib.h>
#include <lualib.h>

static size_t transform(int compress, vectis_zlib_format format,
                        const unsigned char *input, size_t size,
                        unsigned char *output, size_t capacity, uint64_t limit,
                        vectis_status expected, vectis_zlib_result *result) {
  vectis_zlib_options options;
  vectis_zlib *receiver;
  vectis_error error;
  vectis_status status;
  unsigned char chunk[7];
  size_t offset;
  size_t total;
  size_t consumed;
  size_t produced;
  size_t count;
  vectis_zlib_options_init(&options);
  assert(options.max_output_bytes == UINT64_MAX);
  options.compress = compress;
  options.format = format;
  options.max_output_bytes = limit;
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  offset = 0u;
  total = 0u;
  for (;;) {
    /* One-byte input fragments, seven-byte output, no fixture-sized buffer. */
    count = offset < size ? 1u : 0u;
    status = receiver->process(
        receiver, input + offset, count, &consumed, chunk, sizeof(chunk),
        &produced, offset == size ? VECTIS_ZLIB_FINISH : VECTIS_ZLIB_NO_FLUSH,
        &error);
    if (status != VECTIS_OK) {
      assert(status == expected);
      assert(receiver->result(receiver, result, &error) == expected);
      assert(result->input_bytes == 0u && result->output_bytes == 0u);
      break;
    }
    offset += consumed;
    assert(total + produced <= capacity);
    memcpy(output + total, chunk, produced);
    total += produced;
    assert(receiver->result(receiver, result, &error) == VECTIS_OK);
    if (result->finished) {
      assert(expected == VECTIS_OK);
      assert(result->input_bytes == size && result->output_bytes == total);
      assert(receiver->process(receiver, NULL, 0u, &consumed, chunk,
                               sizeof(chunk), &produced, VECTIS_ZLIB_FINISH,
                               &error) == VECTIS_OK);
      assert(consumed == 0u && produced == 0u);
      assert(receiver->process(receiver, "x", 1u, &consumed, chunk,
                               sizeof(chunk), &produced, VECTIS_ZLIB_FINISH,
                               &error) == VECTIS_ERR_STATE);
      break;
    }
  }
  receiver->close(receiver);
  return total;
}

typedef struct allocator_fixture {
  int fail;
  size_t live;
} allocator_fixture;

static void *test_allocate(void *userdata, void *ptr, size_t old_size,
                           size_t size) {
  allocator_fixture *fixture;
  void *next;
  fixture = (allocator_fixture *)userdata;
  if (size == 0u) {
    if (ptr != NULL)
      --fixture->live;
    free(ptr);
    return NULL;
  }
  if (fixture->fail && (ptr == NULL || size > old_size))
    return NULL;
  next = realloc(ptr, size);
  if (next != NULL && ptr == NULL)
    ++fixture->live;
  return next;
}

static int arm_oom(lua_State *lua) {
  allocator_fixture *fixture;
  void *userdata;
  (void)lua_getallocf(lua, &userdata);
  fixture = (allocator_fixture *)userdata;
  fixture->fail = 1;
  return 0;
}

static void lua_oom_test(const char *script) {
  allocator_fixture fixture;
  lua_State *lua;
  int status;
  memset(&fixture, 0, sizeof(fixture));
  lua = lua_newstate(test_allocate, &fixture, 0u);
  assert(lua != NULL);
  luaL_openlibs(lua);
  lua_newtable(lua);
  vectis_lua_zlib_stream_register(lua);
  lua_setglobal(lua, "zlib");
  lua_pushcfunction(lua, arm_oom);
  lua_setglobal(lua, "arm_oom");
  assert(luaL_loadstring(lua, script) == LUA_OK);
  status = lua_pcall(lua, 0, 0, 0);
  fixture.fail = 0;
  assert(status == LUA_ERRMEM);
  lua_close(lua);
  assert(fixture.live == 0u);
}

int main(void) {
  static const unsigned char binary[] = {0u, 1u, 2u, 255u, 0u, 3u};
  unsigned char packed[256];
  unsigned char empty[64];
  unsigned char members[640];
  unsigned char output[64];
  unsigned char dictionary_stream[128];
  vectis_zlib_result result;
  vectis_zlib_options options;
  vectis_zlib *receiver;
  vectis_zlib_impl *impl;
  vectis_error error;
  size_t packed_size;
  size_t empty_size;
  size_t size;
  size_t consumed;
  size_t produced;
  int format;
  z_stream native;
  uint32_t checksum;
  uint32_t checksum_tail;
  uint32_t combined;
  uint32_t polynomial;
  int checksum_kind;
  for (checksum_kind = 0; checksum_kind < 2; ++checksum_kind) {
    assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)checksum_kind, 123u,
                                NULL, 0u, &checksum, &error) == VECTIS_OK);
    assert(checksum == (checksum_kind == 0 ? 1u : 0u));
    assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)checksum_kind, 123u,
                                "", 0u, &checksum, &error) == VECTIS_OK);
    assert(checksum == 123u);
    assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)checksum_kind,
                                checksum_kind == 0 ? 1u : 0u, "123456789", 9u,
                                &checksum, &error) == VECTIS_OK);
    assert(checksum == (checksum_kind == 0 ? 0x091e01deu : 0xcbf43926u));
    assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)checksum_kind,
                                checksum_kind == 0 ? 1u : 0u, "1234", 4u,
                                &combined, &error) == VECTIS_OK);
    assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)checksum_kind,
                                checksum_kind == 0 ? 1u : 0u, "56789", 5u,
                                &checksum_tail, &error) == VECTIS_OK);
    assert(vectis_zlib_checksum_combine(
               (vectis_zlib_checksum_kind)checksum_kind, combined,
               checksum_tail, 5u, &combined, &error) == VECTIS_OK);
    assert(combined == checksum);
  }
  assert(vectis_zlib_checksum((vectis_zlib_checksum_kind)99, 0u, "", 0u,
                              &checksum, &error) == VECTIS_ERR_INVALID);
  assert(checksum == 0u);
  assert(vectis_zlib_checksum(VECTIS_ZLIB_CRC32, 0u, NULL, 1u, &checksum,
                              &error) == VECTIS_ERR_INVALID);
  assert(vectis_zlib_crc32_combine_gen(5u, &polynomial, &error) == VECTIS_OK);
  assert(vectis_zlib_checksum(VECTIS_ZLIB_CRC32, 0u, "1234", 4u, &checksum,
                              &error) == VECTIS_OK);
  assert(vectis_zlib_crc32_combine_op(checksum, checksum_tail, polynomial) ==
         0xcbf43926u);
  assert(vectis_zlib_crc32_combine_gen((uint64_t)INT64_MAX + 1u, &polynomial,
                                       &error) == VECTIS_ERR_INVALID);
  assert(polynomial == 0u);
  for (format = 0; format < 2; ++format) {
    packed_size =
        transform(1, (vectis_zlib_format)format, binary, sizeof(binary), packed,
                  sizeof(packed), UINT64_MAX, VECTIS_OK, &result);
    assert(result.members == 1u);
    size = transform(0, (vectis_zlib_format)format, packed, packed_size, output,
                     sizeof(output), sizeof(binary), VECTIS_OK, &result);
    assert(size == sizeof(binary) && memcmp(output, binary, size) == 0);
    (void)transform(0, VECTIS_ZLIB_AUTO, packed, packed_size, output,
                    sizeof(output), UINT64_MAX, VECTIS_OK, &result);
    (void)transform(0, (vectis_zlib_format)format, packed, packed_size - 1u,
                    output, sizeof(output), UINT64_MAX, VECTIS_ERR_INVALID,
                    &result);
    packed[packed_size] = 'x';
    (void)transform(0, VECTIS_ZLIB_AUTO, packed, packed_size + 1u, output,
                    sizeof(output), UINT64_MAX, VECTIS_ERR_INVALID, &result);
    (void)transform(0, (vectis_zlib_format)format, packed, packed_size, output,
                    sizeof(output), sizeof(binary) - 1u, VECTIS_ERR_INVALID,
                    &result);
    packed[packed_size - 1u] ^= 1u;
    (void)transform(0, (vectis_zlib_format)format, packed, packed_size, output,
                    sizeof(output), UINT64_MAX, VECTIS_ERR_INVALID, &result);
  }
  packed_size = transform(1, VECTIS_ZLIB_GZIP, binary, sizeof(binary), packed,
                          sizeof(packed), UINT64_MAX, VECTIS_OK, &result);
  empty_size = transform(1, VECTIS_ZLIB_GZIP, binary, 0u, empty, sizeof(empty),
                         UINT64_MAX, VECTIS_OK, &result);
  (void)transform(0, VECTIS_ZLIB_GZIP, empty, empty_size, output,
                  sizeof(output), 0u, VECTIS_OK, &result);
  memcpy(members, empty, empty_size);
  memcpy(members + empty_size, packed, packed_size);
  memcpy(members + empty_size + packed_size, empty, empty_size);
  size = 2u * empty_size + packed_size;
  (void)transform(0, VECTIS_ZLIB_AUTO, members, size, output, sizeof(output),
                  sizeof(binary), VECTIS_OK, &result);
  assert(result.members == 3u);
  (void)transform(0, VECTIS_ZLIB_GZIP, members, size - 1u, output,
                  sizeof(output), UINT64_MAX, VECTIS_ERR_INVALID, &result);

  /* An actual dictionary-dependent fixture, not a mocked zlib return code. */
  memset(&native, 0, sizeof(native));
  assert(deflateInit(&native, 9) == Z_OK);
  assert(deflateSetDictionary(&native, binary, sizeof(binary)) == Z_OK);
  native.next_in = packed;
  memcpy(packed, binary, sizeof(binary));
  native.avail_in = sizeof(binary);
  native.next_out = dictionary_stream;
  native.avail_out = sizeof(dictionary_stream);
  assert(deflate(&native, Z_FINISH) == Z_STREAM_END);
  size = sizeof(dictionary_stream) - native.avail_out;
  assert(deflateEnd(&native) == Z_OK);
  (void)transform(0, VECTIS_ZLIB_ZLIB, dictionary_stream, size, output,
                  sizeof(output), UINT64_MAX, VECTIS_ERR_INVALID, &result);
  vectis_zlib_options_init(&options);
  options.compress = 0;
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  assert(receiver->process(receiver, dictionary_stream, size, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_NO_FLUSH,
                           &error) == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "dictionary") != NULL);
  assert(receiver->process(receiver, NULL, 0u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_FINISH,
                           &error) == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "dictionary") != NULL);
  receiver->close(receiver);

  vectis_zlib_options_init(&options);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  impl->result.input_bytes = UINT64_MAX - 1u;
  assert(receiver->process(receiver, binary, 1u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_NO_FLUSH,
                           &error) == VECTIS_OK);
  assert(impl->result.input_bytes == UINT64_MAX);
  assert(receiver->process(receiver, binary, 1u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_NO_FLUSH,
                           &error) == VECTIS_ERR_STATE);
  assert(strstr(error.message, "UINT64_MAX") != NULL);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  impl->result.output_bytes = 64u * 1024u * 1024u;
  assert(receiver->process(receiver, binary, 1u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_SYNC_FLUSH,
                           &error) == VECTIS_OK);
  assert(impl->result.output_bytes > 64u * 1024u * 1024u);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  impl->result.output_bytes = UINT64_MAX - 8u;
  assert(receiver->process(receiver, NULL, 0u, &consumed, output, 8u, &produced,
                           VECTIS_ZLIB_FINISH, &error) == VECTIS_OK);
  assert(produced == 8u && impl->result.output_bytes == UINT64_MAX);
  assert(receiver->process(receiver, NULL, 0u, &consumed, output, 8u, &produced,
                           VECTIS_ZLIB_FINISH, &error) == VECTIS_OK);
  assert(produced == 0u && impl->result.finished);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  impl->result.output_bytes = UINT64_MAX;
  assert(receiver->process(receiver, binary, 1u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_SYNC_FLUSH,
                           &error) == VECTIS_ERR_STATE);
  assert(strstr(error.message, "UINT64_MAX") != NULL);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  impl->result.members = UINT64_MAX;
  assert(receiver->process(receiver, NULL, 0u, &consumed, output,
                           sizeof(output), &produced, VECTIS_ZLIB_FINISH,
                           &error) == VECTIS_ERR_STATE);
  assert(strstr(error.message, "UINT64_MAX") != NULL);
  receiver->close(receiver);
  /* Early close and invalid options leave no native resources. */
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  assert(receiver->process(receiver, binary, 1u, &consumed, output, 6u,
                           &produced, VECTIS_ZLIB_SYNC_FLUSH,
                           &error) == VECTIS_ERR_INVALID);
  assert(consumed == 0u && produced == 0u);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  assert(receiver->process(receiver, binary, sizeof(binary), &consumed, output,
                           1u, &produced, VECTIS_ZLIB_FINISH,
                           &error) == VECTIS_OK);
  impl = (vectis_zlib_impl *)receiver->impl;
  assert(impl->finish_remaining != 0u);
  assert(receiver->process(receiver, binary, impl->finish_remaining + 1u,
                           &consumed, output, 1u, &produced, VECTIS_ZLIB_FINISH,
                           &error) == VECTIS_ERR_STATE);
  assert(strstr(error.message, "after EOF") != NULL);
  receiver->close(receiver);
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_OK);
  assert(receiver->process(receiver, binary, VECTIS_ZLIB_CHUNK_BYTES + 1u,
                           &consumed, output, sizeof(output), &produced,
                           VECTIS_ZLIB_NO_FLUSH, &error) == VECTIS_ERR_INVALID);
  assert(consumed == 0u && produced == 0u);
  receiver->close(receiver);
  options.format = VECTIS_ZLIB_AUTO;
  assert(vectis_zlib_new(&options, &receiver, &error) == VECTIS_ERR_INVALID);
  assert(receiver == NULL);
  vectis_zlib_close(NULL);

  lua_oom_test("local chunk = string.rep('x', 1000); "
               "zlib.gzip_stream(function() arm_oom(); return chunk end, "
               "function() return true end, {sync_flush=true})");
  lua_oom_test("local done=false; zlib.gzip_stream(function() "
               "if done then arm_oom(); return nil end; done=true; return 'x' "
               "end, function() return true end)");
  lua_oom_test("zlib.gzip_stream(function() return nil end, "
               "function() arm_oom(); return true end)");
  puts("zlib C streaming behavior ok");
  return 0;
}
