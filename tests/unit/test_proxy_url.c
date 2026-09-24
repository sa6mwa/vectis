#include "vectis_proxy_url.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *base, const char *path, const char *query,
                  const char *expected_target, const char *expected_host) {
  char *target;
  char *host;
  vectis_error error;

  target = NULL;
  host = NULL;
  assert(vectis_proxy_target_build(base, path, query, &target, &host, &error) ==
         VECTIS_OK);
  assert(strcmp(target, expected_target) == 0);
  assert(strcmp(host, expected_host) == 0);
  free(target);
  free(host);
}

static void reject(const char *base, const char *path, const char *query) {
  char *target;
  char *host;
  vectis_error error;

  target = (char *)1;
  host = (char *)1;
  assert(vectis_proxy_target_build(base, path, query, &target, &host, &error) ==
         VECTIS_ERR_INVALID);
  assert(target == NULL && host == NULL);
}

int main(void) {
  check("https://upstream.test", "/a%2Fb", "x=1&x=2&v=%2B",
        "/a%2Fb?x=1&x=2&v=%2B", "upstream.test");
  check("http://[::1]:8080/base/", "/hello", NULL, "/base/hello", "[::1]:8080");
  check("https://upstream.test/base", "/", "", "/base/", "upstream.test");
  check("https://upstream.test/", "/raw%3Avalue", "q=a?b", "/raw%3Avalue?q=a?b",
        "upstream.test");
  reject("https://upstream.test", "/a%2e%2e/..", NULL);
  reject("https://upstream.test", "/a%252e", NULL);
  reject("https://upstream.test", "/good", "x=%0d%0aInjected:1");
  reject("https://upstream.test", "/good", "x=%ZZ");
  reject("https://upstream.test", "/good", "x=#fragment");
  reject("https://upstream.test/../base", "/good", NULL);
  reject("ftp://upstream.test", "/good", NULL);
  return 0;
}
