#ifndef VECTIS_POUCH_CRYPTO_H
#define VECTIS_POUCH_CRYPTO_H

#include <stddef.h>
#include <string.h>

#define VECTIS_POUCH_CRYPTO_KEY_ENV "VECTIS_POUCH_CRYPTO_KEY"

/* Only exact query parameter names configure Pouch encryption. Text in the
 * root path, fragment, another parameter's value, or a longer name must not
 * suppress Vectis's encrypted-by-default configuration. */
static int vectis_pouch_endpoint_has_crypto_option(const char *endpoint) {
  const char *query;
  const char *fragment;
  const char *cursor;
  const char *query_end;

  if (endpoint == NULL) {
    return 0;
  }
  query = strchr(endpoint, '?');
  fragment = strchr(endpoint, '#');
  if (query == NULL || (fragment != NULL && fragment < query)) {
    return 0;
  }
  cursor = query + 1;
  query_end = fragment != NULL ? fragment : endpoint + strlen(endpoint);
  while (cursor < query_end) {
    const char *parameter_end;
    const char *equals;
    size_t parameter_size;
    size_t name_size;

    parameter_end = memchr(cursor, '&', (size_t)(query_end - cursor));
    if (parameter_end == NULL) {
      parameter_end = query_end;
    }
    parameter_size = (size_t)(parameter_end - cursor);
    equals = memchr(cursor, '=', parameter_size);
    name_size = equals != NULL ? (size_t)(equals - cursor) : parameter_size;
    if ((name_size == sizeof("pouch_crypto_key") - 1u &&
         memcmp(cursor, "pouch_crypto_key", name_size) == 0) ||
        (name_size == sizeof("pouch_crypto_key_file") - 1u &&
         memcmp(cursor, "pouch_crypto_key_file", name_size) == 0)) {
      return 1;
    }
    cursor = parameter_end < query_end ? parameter_end + 1 : query_end;
  }
  return 0;
}

#endif
