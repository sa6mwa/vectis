#include "vectis_cli_docs_source.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
  vectis_cli_embedded_resource documents[3];
  vectis_docs_pager_source source;
  mdf_source_offset offset;
  unsigned char *data;
  char *expected, chunk[137], heading[256];
  size_t body_size, length, position, amount, i;
  int err, printed;

  body_size = 1200000u;
  data = (unsigned char *)malloc(body_size);
  expected = (char *)malloc(body_size + 1024u);
  assert(data != NULL && expected != NULL);
  for (i = 0u; i < body_size; ++i)
    data[i] = (unsigned char)('a' + i % 26u);
  memset(documents, 0, sizeof(documents));
  documents[0].path = "first.md";
  documents[0].sha256 = "first-digest";
  documents[0].size = body_size;
  documents[1].path = "empty.md";
  documents[1].sha256 = "empty-digest";
  documents[2].path = "last.md";
  documents[2].sha256 = "last-digest";
  documents[2].offset = body_size - 19u;
  documents[2].size = 19u;
  memset(&source, 0, sizeof(source));
  source.documents = documents;
  source.data = data;
  source.count = 3u;
  length = 0u;
  for (i = 0u; i < source.count; ++i) {
    printed = snprintf(heading, sizeof(heading),
                       "# Vectis document: `%s`\n\n**SHA-256:** `%s`\n\n",
                       documents[i].path, documents[i].sha256);
    assert(printed > 0 && (size_t)printed < sizeof(heading));
    memcpy(expected + length, heading, (size_t)printed);
    length += (size_t)printed;
    memcpy(expected + length, data + documents[i].offset, documents[i].size);
    length += documents[i].size;
    memcpy(expected + length, "\n\n", 2u);
    length += 2u;
  }
  position = 0u;
  while (
      (amount = vectis_docs_pager_read(&source, chunk, sizeof(chunk), &err))) {
    assert(err == 0 && amount <= length - position);
    assert(memcmp(chunk, expected + position, amount) == 0);
    position += amount;
  }
  assert(err == 0 && position == length);
  /* Every heading/body/separator boundary, backward seeks and exact EOF. */
  for (i = 0u; i < length; i += i < 100u || i > body_size - 20u ? 1u : 7919u) {
    offset.high = 0u;
    offset.low = (unsigned long)i;
    assert(vectis_docs_pager_seek(&source, &offset) == 0);
    amount = vectis_docs_pager_read(&source, chunk, sizeof(chunk), &err);
    assert(err == 0 &&
           amount == (length - i < sizeof(chunk) ? length - i : sizeof(chunk)));
    assert(memcmp(chunk, expected + i, amount) == 0);
  }
  for (i = body_size - 4u; i < length; ++i) {
    offset.low = (unsigned long)i;
    assert(vectis_docs_pager_seek(&source, &offset) == 0);
    amount = vectis_docs_pager_read(&source, chunk, sizeof(chunk), &err);
    assert(err == 0 && amount <= length - i);
    assert(memcmp(chunk, expected + i, amount) == 0);
  }
  offset.low = (unsigned long)length;
  assert(vectis_docs_pager_seek(&source, &offset) == 0);
  assert(vectis_docs_pager_read(&source, chunk, sizeof(chunk), &err) == 0u &&
         err == 0);
  offset.low++;
  assert(vectis_docs_pager_seek(&source, &offset) != 0);
  offset.high = 0xffffffffUL;
  offset.low = 0xffffffffUL;
  assert(vectis_docs_pager_seek(&source, &offset) != 0);
  offset.high = offset.low = 0u;
  assert(vectis_docs_pager_seek(&source, &offset) == 0);
  offset.low = (unsigned long)length + 1u;
  assert(vectis_docs_pager_seek(&source, &offset) != 0);
  assert(vectis_docs_pager_read(&source, chunk, 1u, &err) == 1u &&
         chunk[0] == expected[0]);
  assert(vectis_docs_pager_read(&source, chunk, 0u, &err) == 0u && err == 0);
  assert(vectis_docs_pager_read(&source, NULL, 1u, &err) == 0u && err != 0);
  assert(vectis_docs_pager_seek(&source, NULL) != 0);
  source.count = 0u;
  offset.low = 0u;
  assert(vectis_docs_pager_seek(&source, &offset) == 0);
  assert(vectis_docs_pager_read(&source, chunk, sizeof(chunk), &err) == 0u &&
         err == 0);
  free(expected);
  free(data);
  puts("embedded docs bounded reads and absolute seeks passed");
  return 0;
}
