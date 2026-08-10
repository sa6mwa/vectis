#include <vectis/totp_qr.h>

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VECTIS_QR_MATRIX_MAX (69u * 69u)
#define VECTIS_QR_CODEWORDS_MAX 536u

typedef enum vectis_qr_ecc {
  VECTIS_QR_ECC_LOW = 0,
  VECTIS_QR_ECC_MEDIUM = 1
} vectis_qr_ecc;

static const unsigned char vectis_qr_ecc_words[][VECTIS_QR_MAX_VERSION + 1u] = {
    {0u, 7u, 10u, 15u, 20u, 26u, 18u, 20u, 24u, 30u, 18u, 20u, 24u, 26u},
    {0u, 10u, 16u, 26u, 18u, 24u, 16u, 18u, 22u, 22u, 26u, 30u, 22u, 22u}};
static const unsigned char vectis_qr_blocks[][VECTIS_QR_MAX_VERSION + 1u] = {
    {0u, 1u, 1u, 1u, 1u, 1u, 2u, 2u, 2u, 2u, 4u, 4u, 4u, 4u},
    {0u, 1u, 1u, 1u, 2u, 2u, 4u, 4u, 4u, 5u, 5u, 5u, 8u, 9u}};
static const unsigned char vectis_qr_format_ecc[] = {1u, 0u};

static unsigned int vectis_qr_matrix_size(unsigned int version) {
  return version * 4u + 17u;
}

static unsigned int vectis_qr_raw_codewords(unsigned int version) {
  unsigned int align;
  unsigned int modules;

  modules = (16u * version + 128u) * version + 64u;
  if (version >= 2u) {
    align = version / 7u + 2u;
    modules -= (25u * align - 10u) * align - 55u;
    if (version >= 7u) {
      modules -= 36u;
    }
  }
  return modules / 8u;
}

static unsigned int vectis_qr_data_codewords(unsigned int version,
                                             vectis_qr_ecc ecc) {
  return vectis_qr_raw_codewords(version) -
         vectis_qr_ecc_words[ecc][version] * vectis_qr_blocks[ecc][version];
}

static void vectis_qr_set(unsigned char *modules, unsigned char *function,
                          unsigned int size, int x, int y, int dark) {
  if (x >= 0 && y >= 0 && (unsigned int)x < size && (unsigned int)y < size) {
    modules[(unsigned int)y * size + (unsigned int)x] = (unsigned char)dark;
    function[(unsigned int)y * size + (unsigned int)x] = 1u;
  }
}

static void vectis_qr_finder(unsigned char *modules, unsigned char *function,
                             unsigned int size, int x, int y) {
  int dx;
  int dy;
  int distance;

  for (dy = -4; dy <= 4; dy++) {
    for (dx = -4; dx <= 4; dx++) {
      distance = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
      vectis_qr_set(modules, function, size, x + dx, y + dy,
                    distance != 2 && distance != 4);
    }
  }
}

static unsigned int vectis_qr_alignment_positions(unsigned int version,
                                                  unsigned int positions[3]) {
  unsigned int count;
  unsigned int i;
  unsigned int position;
  unsigned int step;

  if (version == 1u) {
    return 0u;
  }
  count = version / 7u + 2u;
  step = ((version * 4u + count * 2u + 1u) / (count * 2u - 2u)) * 2u;
  positions[0] = 6u;
  position = vectis_qr_matrix_size(version) - 7u;
  for (i = count - 1u; i > 0u; i--) {
    positions[i] = position;
    position -= step;
  }
  return count;
}

static void vectis_qr_alignment(unsigned char *modules, unsigned char *function,
                                unsigned int size, int x, int y) {
  int dx;
  int dy;
  int distance;

  for (dy = -2; dy <= 2; dy++) {
    for (dx = -2; dx <= 2; dx++) {
      distance = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
      vectis_qr_set(modules, function, size, x + dx, y + dy,
                    distance == 2 || (dx == 0 && dy == 0));
    }
  }
}

static void vectis_qr_format(unsigned char *modules, unsigned char *function,
                             unsigned int size, vectis_qr_ecc ecc,
                             unsigned int mask) {
  unsigned int data;
  unsigned int rem;
  unsigned int bits;
  unsigned int i;

  data = (unsigned int)vectis_qr_format_ecc[ecc] << 3u | mask;
  rem = data;
  for (i = 0u; i < 10u; i++) {
    rem = (rem << 1u) ^ (((rem >> 9u) & 1u) * 0x537u);
  }
  bits = ((data << 10u) | rem) ^ 0x5412u;
  for (i = 0u; i <= 5u; i++) {
    vectis_qr_set(modules, function, size, 8, (int)i, (bits >> i) & 1u);
  }
  vectis_qr_set(modules, function, size, 8, 7, (bits >> 6u) & 1u);
  vectis_qr_set(modules, function, size, 8, 8, (bits >> 7u) & 1u);
  vectis_qr_set(modules, function, size, 7, 8, (bits >> 8u) & 1u);
  for (i = 9u; i < 15u; i++) {
    vectis_qr_set(modules, function, size, (int)(14u - i), 8, (bits >> i) & 1u);
  }
  for (i = 0u; i < 8u; i++) {
    vectis_qr_set(modules, function, size, (int)(size - 1u - i), 8,
                  (bits >> i) & 1u);
  }
  for (i = 8u; i < 15u; i++) {
    vectis_qr_set(modules, function, size, 8, (int)(size - 15u + i),
                  (bits >> i) & 1u);
  }
  vectis_qr_set(modules, function, size, 8, (int)(size - 8u), 1);
}

static void vectis_qr_version(unsigned char *modules, unsigned char *function,
                              unsigned int size, unsigned int version) {
  unsigned int bits;
  unsigned int rem;
  unsigned int i;

  if (version < 7u) {
    return;
  }
  rem = version;
  for (i = 0u; i < 12u; i++) {
    rem = (rem << 1u) ^ (((rem >> 11u) & 1u) * 0x1f25u);
  }
  bits = version << 12u | rem;
  for (i = 0u; i < 18u; i++) {
    vectis_qr_set(modules, function, size, (int)(size - 11u + i % 3u),
                  (int)(i / 3u), (bits >> i) & 1u);
    vectis_qr_set(modules, function, size, (int)(i / 3u),
                  (int)(size - 11u + i % 3u), (bits >> i) & 1u);
  }
}

static void vectis_qr_functions(unsigned char *modules, unsigned char *function,
                                unsigned int version) {
  unsigned int size;
  unsigned int positions[3];
  unsigned int count;
  unsigned int i;
  unsigned int j;

  size = vectis_qr_matrix_size(version);
  vectis_qr_finder(modules, function, size, 3, 3);
  vectis_qr_finder(modules, function, size, (int)size - 4, 3);
  vectis_qr_finder(modules, function, size, 3, (int)size - 4);
  for (i = 0u; i < size; i++) {
    if (function[6u * size + i] == 0u) {
      vectis_qr_set(modules, function, size, (int)i, 6, i % 2u == 0u);
    }
    if (function[i * size + 6u] == 0u) {
      vectis_qr_set(modules, function, size, 6, (int)i, i % 2u == 0u);
    }
  }
  count = vectis_qr_alignment_positions(version, positions);
  for (i = 0u; i < count; i++) {
    for (j = 0u; j < count; j++) {
      if (!((i == 0u && j == 0u) || (i == 0u && j == count - 1u) ||
            (i == count - 1u && j == 0u))) {
        vectis_qr_alignment(modules, function, size, (int)positions[i],
                            (int)positions[j]);
      }
    }
  }
  vectis_qr_format(modules, function, size, VECTIS_QR_ECC_MEDIUM, 0u);
  vectis_qr_version(modules, function, size, version);
}

static unsigned char vectis_qr_multiply(unsigned char x, unsigned char y) {
  unsigned char result;
  unsigned char z;

  result = 0u;
  while (y != 0u) {
    if ((y & 1u) != 0u) {
      result ^= x;
    }
    y >>= 1u;
    z = (unsigned char)(x << 1u);
    if ((x & 0x80u) != 0u) {
      z ^= 0x1du;
    }
    x = z;
  }
  return result;
}

static void vectis_qr_remainder(const unsigned char *data,
                                unsigned int data_len, unsigned char *out,
                                unsigned int degree) {
  unsigned char generator[30];
  unsigned char root;
  unsigned char factor;
  unsigned int i;
  unsigned int j;

  memset(generator, 0, sizeof(generator));
  generator[degree - 1u] = 1u;
  root = 1u;
  for (i = 0u; i < degree; i++) {
    for (j = 0u; j < degree; j++) {
      generator[j] = vectis_qr_multiply(generator[j], root);
      if (j + 1u < degree) {
        generator[j] ^= generator[j + 1u];
      }
    }
    root = vectis_qr_multiply(root, 2u);
  }
  memset(out, 0, degree);
  for (i = 0u; i < data_len; i++) {
    factor = data[i] ^ out[0];
    memmove(out, out + 1u, degree - 1u);
    out[degree - 1u] = 0u;
    for (j = 0u; j < degree; j++) {
      out[j] ^= vectis_qr_multiply(generator[j], factor);
    }
  }
}

static int vectis_totp_base32_value(int ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A';
  }
  if (ch >= 'a' && ch <= 'z') {
    return ch - 'a';
  }
  if (ch >= '2' && ch <= '7') {
    return ch - '2' + 26;
  }
  return -1;
}

static vectis_totp_qr_status vectis_totp_base32_decode(const char *input,
                                                       unsigned char *out,
                                                       size_t out_size,
                                                       size_t *out_len) {
  unsigned int accumulator;
  unsigned int bits;
  size_t encoded_chars;
  size_t offset;
  int padding;
  int value;

  accumulator = 0u;
  bits = 0u;
  encoded_chars = 0u;
  offset = 0u;
  padding = 0;
  while (input != NULL && *input != '\0') {
    if (*input == ' ' || *input == '-' || *input == '\t' || *input == '\r' ||
        *input == '\n') {
      input++;
      continue;
    }
    if (*input == '=') {
      padding = 1;
      input++;
      continue;
    }
    if (padding) {
      return VECTIS_TOTP_QR_INVALID;
    }
    value = vectis_totp_base32_value((unsigned char)*input++);
    if (value < 0) {
      return VECTIS_TOTP_QR_INVALID;
    }
    if (encoded_chars == SIZE_MAX) {
      return VECTIS_TOTP_QR_INVALID;
    }
    encoded_chars++;
    accumulator = (accumulator << 5u) | (unsigned int)value;
    bits += 5u;
    if (bits >= 8u) {
      bits -= 8u;
      if (offset >= out_size) {
        return VECTIS_TOTP_QR_TOO_LARGE;
      }
      out[offset++] = (unsigned char)((accumulator >> bits) & 0xffu);
      accumulator &= bits == 0u ? 0u : (1u << bits) - 1u;
    }
  }
  if (offset == 0u ||
      (encoded_chars % 8u != 0u && encoded_chars % 8u != 2u &&
       encoded_chars % 8u != 4u && encoded_chars % 8u != 5u &&
       encoded_chars % 8u != 7u) ||
      (bits > 0u && accumulator != 0u)) {
    return VECTIS_TOTP_QR_INVALID;
  }
  *out_len = offset;
  return VECTIS_TOTP_QR_OK;
}

static vectis_totp_qr_status
vectis_totp_base32_encode(const unsigned char *input, size_t input_len,
                          char *out, size_t out_size) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  unsigned int accumulator;
  unsigned int bits;
  size_t i;
  size_t offset;

  if (input == NULL || out == NULL ||
      out_size < ((input_len * 8u + 4u) / 5u) + 1u) {
    return VECTIS_TOTP_QR_INVALID;
  }
  accumulator = 0u;
  bits = 0u;
  offset = 0u;
  for (i = 0u; i < input_len; i++) {
    accumulator = (accumulator << 8u) | input[i];
    bits += 8u;
    while (bits >= 5u) {
      bits -= 5u;
      out[offset++] = alphabet[(accumulator >> bits) & 0x1fu];
    }
    accumulator &= bits == 0u ? 0u : (1u << bits) - 1u;
  }
  if (bits > 0u) {
    out[offset++] = alphabet[(accumulator << (5u - bits)) & 0x1fu];
  }
  out[offset] = '\0';
  return VECTIS_TOTP_QR_OK;
}

static vectis_totp_qr_status
vectis_totp_code(const vectis_totp *totp, uint64_t counter,
                 char code[VECTIS_TOTP_CODE_LENGTH + 1u]) {
  unsigned char message[8];
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;
  unsigned int binary;
  unsigned int offset;
  int i;

  if (totp == NULL || totp->key_len == 0u ||
      totp->key_len > VECTIS_TOTP_KEY_MAX || code == NULL) {
    return VECTIS_TOTP_QR_INVALID;
  }
  for (i = 7; i >= 0; i--) {
    message[i] = (unsigned char)(counter & 0xffu);
    counter >>= 8u;
  }
  digest_len = 0u;
  if (HMAC(EVP_sha1(), totp->key, (int)totp->key_len, message, sizeof(message),
           digest, &digest_len) == NULL ||
      digest_len < 20u) {
    return VECTIS_TOTP_QR_CRYPTO;
  }
  offset = digest[digest_len - 1u] & 0x0fu;
  binary = ((unsigned int)(digest[offset] & 0x7fu) << 24u) |
           ((unsigned int)digest[offset + 1u] << 16u) |
           ((unsigned int)digest[offset + 2u] << 8u) |
           (unsigned int)digest[offset + 3u];
  (void)snprintf(code, VECTIS_TOTP_CODE_LENGTH + 1u, "%06u", binary % 1000000u);
  return VECTIS_TOTP_QR_OK;
}

static int vectis_totp_uri_unreserved(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
         (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' ||
         ch == '~';
}

static size_t vectis_totp_uri_encoded_size(const char *value) {
  size_t size;

  size = 0u;
  while (*value != '\0') {
    size += vectis_totp_uri_unreserved((unsigned char)*value) ? 1u : 3u;
    value++;
  }
  return size;
}

static char *vectis_totp_uri_encode(char *out, const char *value) {
  static const char hex[] = "0123456789ABCDEF";
  unsigned char ch;

  while (*value != '\0') {
    ch = (unsigned char)*value++;
    if (vectis_totp_uri_unreserved(ch)) {
      *out++ = (char)ch;
    } else {
      *out++ = '%';
      *out++ = hex[ch >> 4u];
      *out++ = hex[ch & 0x0fu];
    }
  }
  return out;
}

const char *vectis_totp_qr_status_string(vectis_totp_qr_status status) {
  switch (status) {
  case VECTIS_TOTP_QR_OK:
    return "ok";
  case VECTIS_TOTP_QR_INVALID:
    return "invalid input";
  case VECTIS_TOTP_QR_TOO_LARGE:
    return "text does not fit an 80-column terminal QR code";
  case VECTIS_TOTP_QR_NOMEM:
    return "out of memory";
  case VECTIS_TOTP_QR_CRYPTO:
    return "cryptographic operation failed";
  default:
    return "unknown error";
  }
}

vectis_totp_qr_status vectis_totp_init(vectis_totp *totp, const char *secret) {
  vectis_totp_qr_status status;

  if (totp == NULL || secret == NULL) {
    return VECTIS_TOTP_QR_INVALID;
  }
  memset(totp, 0, sizeof(*totp));
  status = vectis_totp_base32_decode(secret, totp->key, sizeof(totp->key),
                                     &totp->key_len);
  if (status != VECTIS_TOTP_QR_OK) {
    return status;
  }
  return vectis_totp_base32_encode(totp->key, totp->key_len, totp->secret,
                                   sizeof(totp->secret));
}

vectis_totp_qr_status
vectis_totp_generate(const vectis_totp *totp, uint64_t unix_seconds,
                     char code[VECTIS_TOTP_CODE_LENGTH + 1u]) {
  return vectis_totp_code(totp, unix_seconds / 30u, code);
}

int vectis_totp_validate(const vectis_totp *totp, const char *code,
                         uint64_t unix_seconds, unsigned int window) {
  uint64_t counter;
  uint64_t candidate;
  char expected[VECTIS_TOTP_CODE_LENGTH + 1u];
  int matched;
  int offset;

  if (totp == NULL || code == NULL || strlen(code) != VECTIS_TOTP_CODE_LENGTH ||
      window > 10u) {
    return 0;
  }
  counter = unix_seconds / 30u;
  matched = 0;
  for (offset = -(int)window; offset <= (int)window; offset++) {
    if (offset < 0 && counter < (uint64_t)(-offset)) {
      continue;
    }
    candidate =
        offset < 0 ? counter - (uint64_t)(-offset) : counter + (uint64_t)offset;
    if (vectis_totp_code(totp, candidate, expected) != VECTIS_TOTP_QR_OK) {
      return 0;
    }
    matched |= CRYPTO_memcmp(expected, code, VECTIS_TOTP_CODE_LENGTH) == 0;
  }
  return matched;
}

vectis_totp_qr_status vectis_totp_uri(const vectis_totp *totp,
                                      const char *label, const char *issuer,
                                      char **out) {
  static const char prefix[] = "otpauth://totp/";
  static const char query[] = "?secret=";
  static const char issuer_prefix[] = "&issuer=";
  size_t label_size;
  size_t issuer_size;
  size_t size;
  char *value;
  char *cursor;

  if (totp == NULL || label == NULL || issuer == NULL || out == NULL ||
      *label == '\0' || *issuer == '\0' || totp->secret[0] == '\0') {
    return VECTIS_TOTP_QR_INVALID;
  }
  label_size = vectis_totp_uri_encoded_size(label);
  issuer_size = vectis_totp_uri_encoded_size(issuer);
  size = sizeof(prefix) - 1u + label_size + sizeof(query) - 1u +
         strlen(totp->secret) + sizeof(issuer_prefix) - 1u + issuer_size + 1u;
  if (size < label_size || size < issuer_size) {
    return VECTIS_TOTP_QR_TOO_LARGE;
  }
  value = (char *)malloc(size);
  if (value == NULL) {
    return VECTIS_TOTP_QR_NOMEM;
  }
  cursor = value;
  memcpy(cursor, prefix, sizeof(prefix) - 1u);
  cursor += sizeof(prefix) - 1u;
  cursor = vectis_totp_uri_encode(cursor, label);
  memcpy(cursor, query, sizeof(query) - 1u);
  cursor += sizeof(query) - 1u;
  memcpy(cursor, totp->secret, strlen(totp->secret));
  cursor += strlen(totp->secret);
  memcpy(cursor, issuer_prefix, sizeof(issuer_prefix) - 1u);
  cursor += sizeof(issuer_prefix) - 1u;
  cursor = vectis_totp_uri_encode(cursor, issuer);
  *cursor = '\0';
  *out = value;
  return VECTIS_TOTP_QR_OK;
}

static void vectis_qr_append_bits(unsigned char *data, unsigned int *bit_len,
                                  unsigned int value, unsigned int count) {
  unsigned int i;

  for (i = count; i > 0u; i--) {
    data[*bit_len >> 3u] |=
        (unsigned char)(((value >> (i - 1u)) & 1u) << (7u - (*bit_len & 7u)));
    (*bit_len)++;
  }
}

static unsigned int vectis_qr_mask_bit(unsigned int mask, unsigned int x,
                                       unsigned int y) {
  switch (mask) {
  case 0u:
    return (x + y) % 2u == 0u;
  case 1u:
    return y % 2u == 0u;
  case 2u:
    return x % 3u == 0u;
  case 3u:
    return (x + y) % 3u == 0u;
  case 4u:
    return (y / 2u + x / 3u) % 2u == 0u;
  case 5u:
    return (x * y) % 2u + (x * y) % 3u == 0u;
  case 6u:
    return ((x * y) % 2u + (x * y) % 3u) % 2u == 0u;
  default:
    return ((x * y) % 3u + (x + y) % 2u) % 2u == 0u;
  }
}

static void vectis_qr_apply_mask(unsigned char *modules,
                                 const unsigned char *function,
                                 unsigned int size, unsigned int mask) {
  unsigned int x;
  unsigned int y;

  for (y = 0u; y < size; y++) {
    for (x = 0u; x < size; x++) {
      if (function[y * size + x] == 0u && vectis_qr_mask_bit(mask, x, y)) {
        modules[y * size + x] ^= 1u;
      }
    }
  }
}

static unsigned long vectis_qr_penalty(const unsigned char *modules,
                                       unsigned int size) {
  unsigned int x;
  unsigned int y;
  unsigned int run;
  unsigned int dark;
  unsigned int total;
  unsigned long result;
  int previous;

  result = 0u;
  dark = 0u;
  total = size * size;
  for (y = 0u; y < size; y++) {
    previous = -1;
    run = 0u;
    for (x = 0u; x < size; x++) {
      if (modules[y * size + x] == (unsigned char)previous)
        run++;
      else {
        if (run >= 5u)
          result += run - 2u;
        previous = modules[y * size + x];
        run = 1u;
      }
      dark += modules[y * size + x] != 0u;
    }
    if (run >= 5u)
      result += run - 2u;
  }
  for (x = 0u; x < size; x++) {
    previous = -1;
    run = 0u;
    for (y = 0u; y < size; y++) {
      if (modules[y * size + x] == (unsigned char)previous)
        run++;
      else {
        if (run >= 5u)
          result += run - 2u;
        previous = modules[y * size + x];
        run = 1u;
      }
    }
    if (run >= 5u)
      result += run - 2u;
  }
  for (y = 0u; y + 1u < size; y++)
    for (x = 0u; x + 1u < size; x++)
      if (modules[y * size + x] == modules[y * size + x + 1u] &&
          modules[y * size + x] == modules[(y + 1u) * size + x] &&
          modules[y * size + x] == modules[(y + 1u) * size + x + 1u])
        result += 3u;
  for (y = 0u; y < size; y++)
    for (x = 0u; x + 10u < size; x++)
      if (modules[y * size + x] && !modules[y * size + x + 1u] &&
          modules[y * size + x + 2u] && modules[y * size + x + 3u] &&
          modules[y * size + x + 4u] && !modules[y * size + x + 5u] &&
          modules[y * size + x + 6u] && !modules[y * size + x + 7u] &&
          !modules[y * size + x + 8u] && !modules[y * size + x + 9u] &&
          !modules[y * size + x + 10u])
        result += 40u;
  for (x = 0u; x < size; x++)
    for (y = 0u; y + 10u < size; y++)
      if (modules[y * size + x] && !modules[(y + 1u) * size + x] &&
          modules[(y + 2u) * size + x] && modules[(y + 3u) * size + x] &&
          modules[(y + 4u) * size + x] && !modules[(y + 5u) * size + x] &&
          modules[(y + 6u) * size + x] && !modules[(y + 7u) * size + x] &&
          !modules[(y + 8u) * size + x] && !modules[(y + 9u) * size + x] &&
          !modules[(y + 10u) * size + x])
        result += 40u;
  result += (unsigned long)(abs((int)(dark * 20u) - (int)(total * 10u)) /
                            (int)total) *
            10u;
  return result;
}

static vectis_totp_qr_status vectis_qr_encode_ecc(vectis_qr *qr,
                                                  const unsigned char *text,
                                                  size_t text_len,
                                                  vectis_qr_ecc ecc) {
  unsigned char modules[VECTIS_QR_MATRIX_MAX];
  unsigned char function[VECTIS_QR_MATRIX_MAX];
  unsigned char data[VECTIS_QR_CODEWORDS_MAX];
  unsigned char blocks[9][VECTIS_QR_CODEWORDS_MAX];
  unsigned char correction[9][30];
  unsigned char interleaved[VECTIS_QR_CODEWORDS_MAX];
  unsigned int version;
  unsigned int size;
  unsigned int data_len;
  unsigned int raw_len;
  unsigned int block_count;
  unsigned int ecc_len;
  unsigned int short_data;
  unsigned int short_blocks;
  unsigned int bit_len;
  unsigned int index;
  unsigned int x;
  unsigned int y;
  unsigned int column;
  unsigned int i;
  unsigned int j;
  unsigned int mask;
  unsigned int best_mask;
  unsigned long best_penalty;
  unsigned long penalty;
  int upward;

  if (qr == NULL || (text == NULL && text_len != 0u) || text_len > 65535u)
    return VECTIS_TOTP_QR_INVALID;
  for (version = 1u; version <= VECTIS_QR_MAX_VERSION; version++) {
    data_len = vectis_qr_data_codewords(version, ecc);
    if (4u + (version < 10u ? 8u : 16u) + text_len * 8u <= data_len * 8u)
      break;
  }
  if (version > VECTIS_QR_MAX_VERSION)
    return VECTIS_TOTP_QR_TOO_LARGE;
  raw_len = vectis_qr_raw_codewords(version);
  block_count = vectis_qr_blocks[ecc][version];
  ecc_len = vectis_qr_ecc_words[ecc][version];
  short_data = data_len / block_count;
  short_blocks = block_count - data_len % block_count;
  memset(data, 0, sizeof(data));
  bit_len = 0u;
  vectis_qr_append_bits(data, &bit_len, 4u, 4u);
  vectis_qr_append_bits(data, &bit_len, (unsigned int)text_len,
                        version < 10u ? 8u : 16u);
  for (i = 0u; i < text_len; i++)
    vectis_qr_append_bits(data, &bit_len, text[i], 8u);
  bit_len += bit_len + 4u <= data_len * 8u ? 4u : data_len * 8u - bit_len;
  while ((bit_len & 7u) != 0u)
    bit_len++;
  for (i = bit_len / 8u; i < data_len; i++)
    data[i] = (i & 1u) == 0u ? 0xecu : 0x11u;
  index = 0u;
  for (i = 0u; i < block_count; i++) {
    unsigned int length = short_data + (i >= short_blocks ? 1u : 0u);
    memcpy(blocks[i], data + index, length);
    vectis_qr_remainder(blocks[i], length, correction[i], ecc_len);
    index += length;
  }
  index = 0u;
  for (j = 0u; j <= short_data; j++)
    for (i = 0u; i < block_count; i++) {
      unsigned int length = short_data + (i >= short_blocks ? 1u : 0u);
      if (j < length)
        interleaved[index++] = blocks[i][j];
    }
  for (j = 0u; j < ecc_len; j++)
    for (i = 0u; i < block_count; i++)
      interleaved[index++] = correction[i][j];
  if (index != raw_len)
    return VECTIS_TOTP_QR_INVALID;
  size = vectis_qr_matrix_size(version);
  memset(modules, 0, sizeof(modules));
  memset(function, 0, sizeof(function));
  vectis_qr_functions(modules, function, version);
  index = 0u;
  column = size - 1u;
  while (column > 0u) {
    if (column == 6u)
      column--;
    upward = ((column + 1u) & 2u) == 0u;
    for (i = 0u; i < size; i++) {
      y = upward ? size - 1u - i : i;
      for (j = 0u; j < 2u; j++) {
        x = column - j;
        if (function[y * size + x] == 0u && index < raw_len * 8u) {
          modules[y * size + x] =
              (interleaved[index >> 3u] >> (7u - (index & 7u))) & 1u;
          index++;
        }
      }
    }
    if (column <= 2u) {
      break;
    }
    column -= 2u;
  }
  if (index != raw_len * 8u) {
    return VECTIS_TOTP_QR_INVALID;
  }
  best_mask = 0u;
  best_penalty = ULONG_MAX;
  for (mask = 0u; mask < 8u; mask++) {
    vectis_qr_apply_mask(modules, function, size, mask);
    vectis_qr_format(modules, function, size, ecc, mask);
    penalty = vectis_qr_penalty(modules, size);
    if (penalty < best_penalty) {
      best_penalty = penalty;
      best_mask = mask;
    }
    vectis_qr_apply_mask(modules, function, size, mask);
  }
  vectis_qr_apply_mask(modules, function, size, best_mask);
  vectis_qr_format(modules, function, size, ecc, best_mask);
  memset(qr, 0, sizeof(*qr));
  qr->size = size;
  for (y = 0u; y < size; y++)
    for (x = 0u; x < size; x++)
      if (modules[y * size + x])
        qr->modules[(y * size + x) >> 3u] |=
            (unsigned char)(1u << (7u - ((y * size + x) & 7u)));
  return VECTIS_TOTP_QR_OK;
}

vectis_totp_qr_status vectis_qr_encode(vectis_qr *qr, const unsigned char *text,
                                       size_t text_len) {
  return vectis_qr_encode_ecc(qr, text, text_len, VECTIS_QR_ECC_MEDIUM);
}

vectis_totp_qr_status vectis_totp_enrollment_qr(const vectis_totp *totp,
                                                const char *label,
                                                const char *issuer,
                                                vectis_qr *qr) {
  vectis_totp_qr_status status;
  char *uri;

  if (qr == NULL) {
    return VECTIS_TOTP_QR_INVALID;
  }
  uri = NULL;
  status = vectis_totp_uri(totp, label, issuer, &uri);
  if (status == VECTIS_TOTP_QR_OK) {
    status = vectis_qr_encode_ecc(qr, (const unsigned char *)uri, strlen(uri),
                                  VECTIS_QR_ECC_LOW);
  }
  vectis_totp_qr_free(uri);
  return status;
}

unsigned int vectis_qr_size(const vectis_qr *qr) {
  if (qr == NULL) {
    return 0u;
  }
  return qr->size;
}

vectis_totp_qr_status vectis_qr_render_ansi(const vectis_qr *qr, char **out,
                                            size_t *out_len) {
  static const char top[] = "\342\226\200";
  static const char bottom[] = "\342\226\204";
  static const char full[] = "\342\226\210";
  unsigned int size;
  unsigned int rendered_size;
  unsigned int x;
  unsigned int y;
  unsigned int rows;
  size_t capacity;
  size_t offset;
  const char *glyph;
  int upper;
  int lower;
  char *value;

  if (qr == NULL || out == NULL || out_len == NULL) {
    return VECTIS_TOTP_QR_INVALID;
  }
  size = vectis_qr_size(qr);
  if (size == 0u ||
      size > VECTIS_QR_TERMINAL_MAX_COLUMNS - VECTIS_QR_QUIET_ZONE * 2u) {
    return VECTIS_TOTP_QR_INVALID;
  }
  rendered_size = size + VECTIS_QR_QUIET_ZONE * 2u;
  rows = (rendered_size + 1u) / 2u;
  capacity = (size_t)rows * ((size_t)rendered_size * 3u + 1u) + 1u;
  value = (char *)malloc(capacity);
  if (value == NULL) {
    return VECTIS_TOTP_QR_NOMEM;
  }
  offset = 0u;
  for (y = 0u; y < rendered_size; y += 2u) {
    for (x = 0u; x < rendered_size; x++) {
      upper = x >= VECTIS_QR_QUIET_ZONE && y >= VECTIS_QR_QUIET_ZONE &&
              x < size + VECTIS_QR_QUIET_ZONE &&
              y < size + VECTIS_QR_QUIET_ZONE &&
              (qr->modules[((y - VECTIS_QR_QUIET_ZONE) * size +
                            (x - VECTIS_QR_QUIET_ZONE)) >>
                           3u] &
               (1u << (7u - (((y - VECTIS_QR_QUIET_ZONE) * size +
                              (x - VECTIS_QR_QUIET_ZONE)) &
                             7u)))) != 0u;
      lower = x >= VECTIS_QR_QUIET_ZONE && y + 1u >= VECTIS_QR_QUIET_ZONE &&
              x < size + VECTIS_QR_QUIET_ZONE &&
              y + 1u < size + VECTIS_QR_QUIET_ZONE &&
              (qr->modules[((y + 1u - VECTIS_QR_QUIET_ZONE) * size +
                            (x - VECTIS_QR_QUIET_ZONE)) >>
                           3u] &
               (1u << (7u - (((y + 1u - VECTIS_QR_QUIET_ZONE) * size +
                              (x - VECTIS_QR_QUIET_ZONE)) &
                             7u)))) != 0u;
      glyph = upper ? (lower ? full : top) : (lower ? bottom : " ");
      memcpy(value + offset, glyph, strlen(glyph));
      offset += strlen(glyph);
    }
    value[offset++] = '\n';
  }
  value[offset] = '\0';
  *out = value;
  *out_len = offset;
  return VECTIS_TOTP_QR_OK;
}

void vectis_totp_qr_free(void *value) { free(value); }
