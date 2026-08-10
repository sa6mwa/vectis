#ifndef VECTIS_TOTP_QR_H
#define VECTIS_TOTP_QR_H

#include <stddef.h>
#include <stdint.h>

#define VECTIS_TOTP_CODE_LENGTH 6u
#define VECTIS_TOTP_SECRET_MAX 256u
#define VECTIS_TOTP_KEY_MAX 128u
#define VECTIS_QR_QUIET_ZONE 4u
#define VECTIS_QR_TERMINAL_MAX_COLUMNS 80u
#define VECTIS_QR_MAX_VERSION 13u
#define VECTIS_QR_BUFFER_LEN 597u

typedef enum vectis_totp_qr_status {
  VECTIS_TOTP_QR_OK = 0,
  VECTIS_TOTP_QR_INVALID,
  VECTIS_TOTP_QR_TOO_LARGE,
  VECTIS_TOTP_QR_NOMEM,
  VECTIS_TOTP_QR_CRYPTO
} vectis_totp_qr_status;

typedef struct vectis_totp {
  unsigned char key[VECTIS_TOTP_KEY_MAX];
  size_t key_len;
  char secret[VECTIS_TOTP_SECRET_MAX];
} vectis_totp;

typedef struct vectis_qr {
  unsigned int size;
  unsigned char modules[VECTIS_QR_BUFFER_LEN];
} vectis_qr;

const char *vectis_totp_qr_status_string(vectis_totp_qr_status status);
vectis_totp_qr_status vectis_totp_init(vectis_totp *totp, const char *secret);
vectis_totp_qr_status
vectis_totp_generate(const vectis_totp *totp, uint64_t unix_seconds,
                     char code[VECTIS_TOTP_CODE_LENGTH + 1u]);
int vectis_totp_validate(const vectis_totp *totp, const char *code,
                         uint64_t unix_seconds, unsigned int window);
vectis_totp_qr_status vectis_totp_uri(const vectis_totp *totp,
                                      const char *label, const char *issuer,
                                      char **out);
vectis_totp_qr_status vectis_totp_enrollment_qr(const vectis_totp *totp,
                                                const char *label,
                                                const char *issuer,
                                                vectis_qr *qr);

vectis_totp_qr_status vectis_qr_encode(vectis_qr *qr, const unsigned char *text,
                                       size_t text_len);
unsigned int vectis_qr_size(const vectis_qr *qr);
vectis_totp_qr_status vectis_qr_render_ansi(const vectis_qr *qr, char **out,
                                            size_t *out_len);
void vectis_totp_qr_free(void *value);

#endif
