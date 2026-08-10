#include <vectis/totp_qr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed: %s (%s:%d)\n", #expr, __FILE__,           \
              __LINE__);                                                       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static uint64_t qr_hash(const vectis_qr *qr) {
  uint64_t value;
  size_t i;
  size_t length;

  value = UINT64_C(0xcbf29ce484222325);
  length = ((size_t)qr->size * qr->size + 7u) / 8u;
  for (i = 0u; i < length; i++) {
    value ^= qr->modules[i];
    value *= UINT64_C(0x100000001b3);
  }
  return value;
}

static int check_qr_hash(const vectis_qr *qr, uint64_t expected) {
  uint64_t actual;

  actual = qr_hash(qr);
  if (actual != expected) {
    fprintf(stderr, "qr hash: 0x%016llx\n", (unsigned long long)actual);
    return 0;
  }
  return 1;
}

int main(void) {
  static const char secret[] = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
  static const char expected_uri[] =
      "otpauth://totp/Vectis%3Astats?secret="
      "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Vectis";
  vectis_totp totp;
  vectis_qr qr;
  vectis_totp_qr_status status;
  char code[VECTIS_TOTP_CODE_LENGTH + 1u];
  char *uri;
  char *rendered;
  size_t rendered_len;
  unsigned char oversized[400];

  status = vectis_totp_init(&totp, secret);
  CHECK(status == VECTIS_TOTP_QR_OK);
  CHECK(strcmp(totp.secret, secret) == 0);
  status = vectis_totp_generate(&totp, 59u, code);
  CHECK(status == VECTIS_TOTP_QR_OK);
  CHECK(strcmp(code, "287082") == 0);
  CHECK(vectis_totp_validate(&totp, "287082", 59u, 0u));
  CHECK(!vectis_totp_validate(&totp, "287083", 59u, 0u));
  CHECK(vectis_totp_validate(&totp, "287082", 89u, 1u));
  uri = NULL;
  status = vectis_totp_uri(&totp, "Vectis:stats", "Vectis", &uri);
  CHECK(status == VECTIS_TOTP_QR_OK);
  CHECK(strcmp(uri, expected_uri) == 0);
  status = vectis_totp_enrollment_qr(&totp, "Vectis:stats", "Vectis", &qr);
  CHECK(status == VECTIS_TOTP_QR_OK);
  CHECK(vectis_qr_size(&qr) == 37u);
  /* Frozen canonical output for the compact Google Key URI profile. */
  CHECK(check_qr_hash(&qr, UINT64_C(0x462ba8e44c149637)));
  rendered = NULL;
  rendered_len = 0u;
  status = vectis_qr_render_ansi(&qr, &rendered, &rendered_len);
  CHECK(status == VECTIS_TOTP_QR_OK);
  CHECK(rendered_len > 0u);
  CHECK(strstr(rendered, "\342\226\210") != NULL);
  CHECK(rendered[rendered_len - 1u] == '\n');
  vectis_totp_qr_free(rendered);
  vectis_totp_qr_free(uri);
  memset(oversized, 'A', sizeof(oversized));
  CHECK(vectis_qr_encode(&qr, oversized, sizeof(oversized)) ==
        VECTIS_TOTP_QR_TOO_LARGE);
  CHECK(vectis_totp_init(&totp, "bad!") == VECTIS_TOTP_QR_INVALID);
  return 0;
}
