#include "../../src/vectis_digest.c"
#include <assert.h>

typedef struct digest_reader_fixture {
  size_t offset;
  int fail;
  int oversize;
} digest_reader_fixture;

static vectis_status fixture_read(void *userdata, unsigned char *buffer,
                                  size_t capacity, size_t *size,
                                  vectis_error *error) {
  digest_reader_fixture *fixture;
  (void)error;
  fixture = (digest_reader_fixture *)userdata;
  assert(capacity == VECTIS_DIGEST_CHUNK_BYTES);
  if (fixture->fail)
    return VECTIS_ERR_STATE;
  if (fixture->oversize) {
    *size = capacity + 1u;
    return VECTIS_OK;
  }
  *size = fixture->offset < 3u ? 1u : 0u;
  if (*size != 0u)
    buffer[0] = (unsigned char)"abc"[fixture->offset++];
  return VECTIS_OK;
}

int main(void) {
  static const unsigned char abc[] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  vectis_digest *receiver;
  vectis_digest_impl *impl;
  vectis_digest_result result;
  vectis_digest_result again;
  vectis_error error;
  digest_reader_fixture fixture;
  char path[4096];
  FILE *file;
  memset(&fixture, 0, sizeof(fixture));
  assert(vectis_digest_stream("sha256", fixture_read, &fixture, &result,
                              &error) == VECTIS_OK);
  assert(result.size == sizeof(abc) && result.bytes == 3u);
  assert(memcmp(result.data, abc, sizeof(abc)) == 0);
  assert(vectis_digest_materialized("sha256", "abc", 3u, &again, &error) ==
         VECTIS_OK);
  assert(memcmp(&result, &again, sizeof(result)) == 0);
  fixture.fail = 1;
  assert(vectis_digest_stream("sha256", fixture_read, &fixture, &result,
                              &error) == VECTIS_ERR_STATE);
  assert(result.size == 0u);
  fixture.fail = 0;
  fixture.oversize = 1;
  assert(vectis_digest_stream("sha256", fixture_read, &fixture, &result,
                              &error) == VECTIS_ERR_INVALID);
  assert(result.size == 0u);
  assert(vectis_digest_new("not-a-digest", &receiver, &error) ==
         VECTIS_ERR_INVALID);
  assert(receiver == NULL);
  assert(vectis_digest_new("shake128", &receiver, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_digest_new("sha256", &receiver, &error) == VECTIS_OK);
  assert(receiver->update(receiver, "a", 1u, &error) == VECTIS_OK);
  assert(receiver->update(receiver, "bc", 2u, &error) == VECTIS_OK);
  assert(receiver->final(receiver, &result, &error) == VECTIS_OK);
  assert(memcmp(result.data, abc, sizeof(abc)) == 0);
  assert(receiver->final(receiver, &again, &error) == VECTIS_OK);
  assert(memcmp(&result, &again, sizeof(result)) == 0);
  assert(receiver->update(receiver, "x", 1u, &error) == VECTIS_ERR_STATE);
  receiver->close(receiver);

  /* Test accounting boundaries with three actual bytes, not exabytes of IO. */
  assert(vectis_digest_new("sha512", &receiver, &error) == VECTIS_OK);
  impl = (vectis_digest_impl *)receiver->impl;
  impl->result.bytes = UINT64_MAX - 3u;
  assert(receiver->update(receiver, "abc", 3u, &error) == VECTIS_OK);
  assert(impl->result.bytes == UINT64_MAX);
  assert(receiver->update(receiver, "x", 1u, &error) == VECTIS_ERR_STATE);
  assert(strstr(error.message, "UINT64_MAX") != NULL);
  assert(receiver->final(receiver, &result, &error) == VECTIS_ERR_STATE);
  assert(result.size == 0u);
  receiver->close(receiver);
  vectis_digest_close(NULL);

  assert(vectis_hmac_materialized("sha256", "k\0ey", 4u, "abc", 3u, &again,
                                  &error) == VECTIS_OK);
  memset(&fixture, 0, sizeof(fixture));
  assert(vectis_hmac_stream("sha256", "k\0ey", 4u, fixture_read, &fixture,
                            &result, &error) == VECTIS_OK);
  assert(memcmp(&result, &again, sizeof(result)) == 0);
  fixture.fail = 1;
  assert(vectis_hmac_stream("sha256", "key", 3u, fixture_read, &fixture,
                            &result, &error) == VECTIS_ERR_STATE);
  assert(result.size == 0u);
  fixture.fail = 0;
  fixture.oversize = 1;
  assert(vectis_hmac_stream("sha256", "key", 3u, fixture_read, &fixture,
                            &result, &error) == VECTIS_ERR_INVALID);
  assert(vectis_hmac_new("shake128", "", 0u, &receiver, &error) ==
         VECTIS_ERR_INVALID);
  assert(receiver == NULL);
  assert(vectis_hmac_new("sha256", NULL, 1u, &receiver, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_hmac_new("sha512", NULL, 0u, &receiver, &error) == VECTIS_OK);
  impl = (vectis_digest_impl *)receiver->impl;
  impl->result.bytes = UINT64_MAX - 3u;
  assert(receiver->update(receiver, "abc", 3u, &error) == VECTIS_OK);
  assert(receiver->update(receiver, "x", 1u, &error) == VECTIS_ERR_STATE);
  assert(receiver->final(receiver, &result, &error) == VECTIS_ERR_STATE);
  assert(result.size == 0u);
  receiver->close(receiver);

  assert(strlen(VECTIS_DIGEST_TEST_ROOT) + 20u < sizeof(path));
  strcpy(path, VECTIS_DIGEST_TEST_ROOT);
  strcat(path, "/digest-fixture.bin");
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite("abc", 1u, 3u, file) == 3u);
  assert(fclose(file) == 0);
  assert(vectis_digest_file("sha256", path, &result, &error) == VECTIS_OK);
  assert(memcmp(result.data, abc, sizeof(abc)) == 0);
  assert(vectis_hmac_file("sha256", "k\0ey", 4u, path, &result, &error) ==
         VECTIS_OK);
  assert(memcmp(&result, &again, sizeof(result)) == 0);
  assert(remove(path) == 0);
  assert(vectis_digest_file("sha256", path, &result, &error) != VECTIS_OK);
  assert(result.size == 0u);
  assert(vectis_hmac_file("sha256", "key", 3u, path, &result, &error) !=
         VECTIS_OK);
  assert(result.size == 0u);
  assert(vectis_digest_file("sha256", VECTIS_DIGEST_TEST_ROOT, &result,
                            &error) != VECTIS_OK);
  return 0;
}
