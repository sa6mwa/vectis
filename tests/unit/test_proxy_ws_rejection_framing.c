#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Test-only HTTP/1.1 framer for the connect-only WebSocket rejection path.
 * The input buffer stays with the caller; only metadata is retained here. */
#define HEAD_LIMIT 16384
#define TRAILER_LIMIT 8192
#define CHUNK_LINE_LIMIT 256

enum phase {
  HEAD,
  FIXED_BODY,
  CLOSE_BODY,
  CHUNK_LINE,
  CHUNK_BODY,
  CHUNK_CR,
  CHUNK_LF,
  TRAILERS,
  UPGRADE,
  DONE,
  BAD
};

enum event {
  NEED_INPUT,
  BODY,
  INTERIM,
  FINAL,
  SWITCHING,
  COMPLETE,
  PAUSED,
  INVALID
};

struct framer {
  enum phase phase;
  unsigned status;
  unsigned interim_count;
  size_t remaining;
  size_t head_used;
  size_t trailer_used;
  size_t chunk_line_used;
  int has_length;
  int has_transfer;
  char head[HEAD_LIMIT + 1];
  char trailers[TRAILER_LIMIT + 1];
  char chunk_line[CHUNK_LINE_LIMIT + 1];
};

struct step {
  enum event event;
  size_t consumed;
  size_t body_offset;
  size_t body_length;
};

static int token(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z'))
    return 1;
  return strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

static int field(const char *line, size_t len, size_t *name_len) {
  size_t i;

  for (i = 0; i < len && line[i] != ':'; i++) {
    if (!token((unsigned char)line[i]))
      return 0;
  }
  if (i == 0 || i == len)
    return 0;
  *name_len = i;
  for (i++; i < len; i++) {
    unsigned char c;

    c = (unsigned char)line[i];
    if (c == 0 || c == '\r' || c == '\n' || (c < 32 && c != '\t') || c == 127)
      return 0;
  }
  return 1;
}

static int decimal(const char *start, const char *end, size_t *value) {
  size_t n;
  const char *p;

  if (start == end)
    return 0;
  n = 0;
  for (p = start; p < end; p++) {
    if (*p < '0' || *p > '9' || n > (SIZE_MAX - (size_t)(*p - '0')) / 10)
      return 0;
    n = n * 10 + (size_t)(*p - '0');
  }
  *value = n;
  return 1;
}

static int parse_head(struct framer *p) {
  size_t pos;
  size_t end;
  size_t name_len;
  size_t length;

  if (p->head_used < 17 || memcmp(p->head, "HTTP/1.1 ", 9) != 0)
    return 0;
  if (p->head[9] < '1' || p->head[9] > '5' || p->head[10] < '0' ||
      p->head[10] > '9' || p->head[11] < '0' || p->head[11] > '9' ||
      p->head[12] != ' ')
    return 0;
  p->status = (unsigned)(p->head[9] - '0') * 100U +
              (unsigned)(p->head[10] - '0') * 10U +
              (unsigned)(p->head[11] - '0');
  pos = 13;
  while (pos + 1 < p->head_used &&
         !(p->head[pos] == '\r' && p->head[pos + 1] == '\n')) {
    unsigned char c;

    c = (unsigned char)p->head[pos++];
    if (c < 32 || c == 127)
      return 0;
  }
  if (pos + 1 >= p->head_used)
    return 0;
  pos += 2;
  p->has_length = 0;
  p->has_transfer = 0;
  while (pos + 1 < p->head_used) {
    end = pos;
    while (end + 1 < p->head_used &&
           !(p->head[end] == '\r' && p->head[end + 1] == '\n'))
      end++;
    if (end + 1 >= p->head_used)
      return 0;
    if (end == pos) {
      if (end + 2 != p->head_used)
        return 0;
      break;
    }
    if (!field(p->head + pos, end - pos, &name_len))
      return 0;
    if (name_len == 14 &&
        strncasecmp(p->head + pos, "Content-Length", 14) == 0) {
      const char *first;
      const char *last;

      if (p->has_length)
        return 0;
      first = p->head + pos + name_len + 1;
      last = p->head + end;
      while (first < last && (*first == ' ' || *first == '\t'))
        first++;
      while (last > first && (last[-1] == ' ' || last[-1] == '\t'))
        last--;
      if (!decimal(first, last, &length))
        return 0;
      p->remaining = length;
      p->has_length = 1;
    } else if (name_len == 17 &&
               strncasecmp(p->head + pos, "Transfer-Encoding", 17) == 0) {
      const char *first;
      const char *last;

      if (p->has_transfer)
        return 0;
      first = p->head + pos + name_len + 1;
      last = p->head + end;
      while (first < last && (*first == ' ' || *first == '\t'))
        first++;
      while (last > first && (last[-1] == ' ' || last[-1] == '\t'))
        last--;
      if ((size_t)(last - first) != 7 || strncasecmp(first, "chunked", 7) != 0)
        return 0;
      p->has_transfer = 1;
    }
    pos = end + 2;
  }
  if (p->has_transfer && p->has_length)
    return 0;
  if (p->status < 200) {
    if (p->has_transfer || p->has_length)
      return 0;
    if (p->status == 101) {
      p->phase = UPGRADE;
    } else {
      p->interim_count++;
      if (p->interim_count > 8)
        return 0;
      p->phase = HEAD;
      p->head_used = 0;
    }
  } else if (p->status == 204 || p->status == 304) {
    if (p->status == 204 && (p->has_length || p->has_transfer))
      return 0;
    p->phase = DONE;
  } else if (p->has_transfer) {
    p->phase = CHUNK_LINE;
  } else if (p->has_length) {
    p->phase = p->remaining == 0 ? DONE : FIXED_BODY;
  } else {
    p->phase = CLOSE_BODY;
  }
  return 1;
}

static int parse_chunk_line(struct framer *p) {
  size_t i;
  size_t value;
  unsigned digit;

  if (p->chunk_line_used < 3)
    return 0;
  value = 0;
  for (i = 0; i + 2 < p->chunk_line_used; i++) {
    unsigned char c;

    c = (unsigned char)p->chunk_line[i];
    if (c == ';')
      break;
    if (c >= '0' && c <= '9')
      digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      digit = (unsigned)(c - 'A' + 10);
    else
      return 0;
    if (value > (SIZE_MAX - digit) / 16)
      return 0;
    value = value * 16 + digit;
  }
  if (i == 0)
    return 0;
  while (i + 2 < p->chunk_line_used) {
    unsigned char c;

    c = (unsigned char)p->chunk_line[i++];
    if (c < 32 || c == 127)
      return 0;
  }
  p->remaining = value;
  p->chunk_line_used = 0;
  p->phase = value == 0 ? TRAILERS : CHUNK_BODY;
  return 1;
}

static int parse_trailers(struct framer *p) {
  size_t pos;
  size_t end;
  size_t name_len;

  if (p->trailer_used == 2)
    return 1;
  pos = 0;
  while (pos + 1 < p->trailer_used) {
    end = pos;
    while (end + 1 < p->trailer_used &&
           !(p->trailers[end] == '\r' && p->trailers[end + 1] == '\n'))
      end++;
    if (end + 1 >= p->trailer_used)
      return 0;
    if (end == pos)
      return end + 2 == p->trailer_used;
    if (!field(p->trailers + pos, end - pos, &name_len))
      return 0;
    if ((name_len == 14 &&
         strncasecmp(p->trailers + pos, "Content-Length", 14) == 0) ||
        (name_len == 17 &&
         strncasecmp(p->trailers + pos, "Transfer-Encoding", 17) == 0) ||
        (name_len == 10 &&
         strncasecmp(p->trailers + pos, "Connection", 10) == 0))
      return 0;
    pos = end + 2;
  }
  return 0;
}

static struct step feed(struct framer *p, const unsigned char *data,
                        size_t length, size_t budget) {
  struct step out;
  size_t n;

  memset(&out, 0, sizeof(out));
  out.event = NEED_INPUT;
  if (p->phase == DONE || p->phase == BAD || p->phase == UPGRADE) {
    out.event = p->phase == DONE      ? COMPLETE
                : p->phase == UPGRADE ? SWITCHING
                                      : INVALID;
    return out;
  }
  while (out.consumed < length) {
    unsigned char c;

    c = data[out.consumed];
    if (p->phase == HEAD) {
      if (p->head_used == HEAD_LIMIT)
        goto bad;
      p->head[p->head_used++] = (char)c;
      out.consumed++;
      if (p->head_used >= 2 && p->head[p->head_used - 2] == '\r' && c == '\n' &&
          p->head_used == 2)
        goto bad;
      if (p->head_used >= 4 &&
          memcmp(p->head + p->head_used - 4, "\r\n\r\n", 4) == 0) {
        if (!parse_head(p))
          goto bad;
        out.event = p->phase == HEAD      ? INTERIM
                    : p->phase == UPGRADE ? SWITCHING
                                          : FINAL;
        return out;
      }
    } else if (p->phase == FIXED_BODY || p->phase == CLOSE_BODY ||
               p->phase == CHUNK_BODY) {
      if (budget == 0) {
        out.event = PAUSED;
        return out;
      }
      n = length - out.consumed;
      if (n > budget)
        n = budget;
      if (p->phase != CLOSE_BODY && n > p->remaining)
        n = p->remaining;
      out.body_offset = out.consumed;
      out.body_length = n;
      out.consumed += n;
      if (p->phase != CLOSE_BODY) {
        p->remaining -= n;
        if (p->remaining == 0)
          p->phase = p->phase == FIXED_BODY ? DONE : CHUNK_CR;
      }
      out.event = BODY;
      return out;
    } else if (p->phase == CHUNK_LINE) {
      if (p->chunk_line_used == CHUNK_LINE_LIMIT)
        goto bad;
      p->chunk_line[p->chunk_line_used++] = (char)c;
      out.consumed++;
      if (p->chunk_line_used >= 2 &&
          p->chunk_line[p->chunk_line_used - 2] == '\r' && c == '\n') {
        if (!parse_chunk_line(p))
          goto bad;
      }
    } else if (p->phase == CHUNK_CR) {
      if (c != '\r')
        goto bad;
      out.consumed++;
      p->phase = CHUNK_LF;
    } else if (p->phase == CHUNK_LF) {
      if (c != '\n')
        goto bad;
      out.consumed++;
      p->phase = CHUNK_LINE;
    } else if (p->phase == TRAILERS) {
      if (p->trailer_used == TRAILER_LIMIT)
        goto bad;
      p->trailers[p->trailer_used++] = (char)c;
      out.consumed++;
      if ((p->trailer_used == 2 && memcmp(p->trailers, "\r\n", 2) == 0) ||
          (p->trailer_used >= 4 &&
           memcmp(p->trailers + p->trailer_used - 4, "\r\n\r\n", 4) == 0)) {
        if (!parse_trailers(p))
          goto bad;
        p->phase = DONE;
        out.event = COMPLETE;
        return out;
      }
    }
  }
  return out;
bad:
  p->phase = BAD;
  out.event = INVALID;
  return out;
}

static enum event at_eof(struct framer *p) {
  if (p->phase == CLOSE_BODY || p->phase == DONE) {
    p->phase = DONE;
    return COMPLETE;
  }
  p->phase = BAD;
  return INVALID;
}

static void test_fixed(void) {
  static const char head[] =
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
      "HTTP/1.1 403 Forbidden\r\nContent-Length: 1048576\r\n\r\n";
  struct framer p;
  struct step step;
  unsigned char body[8192];
  size_t offset;
  size_t sent;
  size_t i;

  memset(&p, 0, sizeof(p));
  for (offset = 0; offset < sizeof(head) - 1;) {
    step = feed(&p, (const unsigned char *)head + offset, 1, 0);
    assert(step.consumed == 1);
    if (step.event == INTERIM)
      assert(p.status == 103);
    if (step.event == FINAL)
      assert(p.status == 403 && p.phase == FIXED_BODY);
    assert(step.event != INVALID);
    offset++;
  }
  assert(p.interim_count == 1 && p.phase == FIXED_BODY);
  memset(body, 'x', sizeof(body));
  step = feed(&p, body, sizeof(body), 0);
  assert(step.event == PAUSED && step.consumed == 0);
  sent = 0;
  while (sent < 1024U * 1024U) {
    offset = 0;
    while (offset < sizeof(body)) {
      step = feed(&p, body + offset, sizeof(body) - offset, 512);
      assert(step.event == BODY && step.body_length <= 512);
      for (i = 0; i < step.body_length; i++)
        assert(body[offset + step.body_offset + i] == 'x');
      offset += step.consumed;
      sent += step.body_length;
    }
  }
  assert(sent == 1024U * 1024U && p.phase == DONE);
  assert(at_eof(&p) == COMPLETE);
}

static void test_chunked(void) {
  static const char response[] =
      "HTTP/1.1 103 Early Hints\r\n\r\n"
      "HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n"
      "4;test=yes\r\ndeny\r\n"
      "2\r\n!\n\r\n"
      "0\r\nX-Trace: yes\r\n\r\n";
  struct framer p;
  struct step step;
  char output[8];
  size_t offset;
  size_t used;

  memset(&p, 0, sizeof(p));
  used = 0;
  for (offset = 0; offset < sizeof(response) - 1;) {
    step = feed(&p, (const unsigned char *)response + offset, 1, 1);
    assert(step.consumed == 1);
    if (step.event == BODY)
      output[used++] = response[offset + step.body_offset];
    assert(step.event != INVALID);
    offset++;
  }
  assert(p.phase == DONE && p.interim_count == 1 && p.status == 403);
  assert(used == 6 && memcmp(output, "deny!\n", 6) == 0);
  assert(p.trailer_used == sizeof("X-Trace: yes\r\n\r\n") - 1);
  assert(at_eof(&p) == COMPLETE);
}

static void test_large_chunked(void) {
  static const char head[] =
      "HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n";
  static const char size_line[] = "100000\r\n";
  static const char tail[] = "\r\n0\r\n\r\n";
  struct framer p;
  struct step step;
  unsigned char body[8192];
  size_t offset;
  size_t total;

  memset(&p, 0, sizeof(p));
  step = feed(&p, (const unsigned char *)head, sizeof(head) - 1, 0);
  assert(step.event == FINAL && step.consumed == sizeof(head) - 1);
  step = feed(&p, (const unsigned char *)size_line, sizeof(size_line) - 1, 0);
  assert(step.event == NEED_INPUT && p.phase == CHUNK_BODY);
  memset(body, 'y', sizeof(body));
  total = 0;
  while (total < 1024U * 1024U) {
    offset = 0;
    while (offset < sizeof(body)) {
      step = feed(&p, body + offset, sizeof(body) - offset, 512);
      assert(step.event == BODY && step.body_length <= 512);
      assert(step.body_length == step.consumed);
      offset += step.consumed;
      total += step.body_length;
    }
  }
  assert(p.phase == CHUNK_CR && total == 1024U * 1024U);
  step = feed(&p, (const unsigned char *)tail, sizeof(tail) - 1, 0);
  assert(step.event == COMPLETE && step.consumed == sizeof(tail) - 1);
  assert(at_eof(&p) == COMPLETE);
}

static void test_close_and_upgrade(void) {
  static const char close_response[] =
      "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nbody";
  static const char upgrade[] =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\nframe";
  struct framer p;
  struct step step;
  size_t offset;

  memset(&p, 0, sizeof(p));
  offset = 0;
  step = feed(&p, (const unsigned char *)close_response,
              sizeof(close_response) - 1, 4);
  assert(step.event == FINAL && p.phase == CLOSE_BODY);
  offset += step.consumed;
  step = feed(&p, (const unsigned char *)close_response + offset,
              sizeof(close_response) - 1 - offset, 4);
  assert(step.event == BODY && step.body_length == 4);
  assert(at_eof(&p) == COMPLETE);

  memset(&p, 0, sizeof(p));
  step = feed(&p, (const unsigned char *)upgrade, sizeof(upgrade) - 1, 4);
  assert(step.event == SWITCHING && p.phase == UPGRADE);
  assert(sizeof(upgrade) - 1 - step.consumed == 5);
}

static void test_invalid(void) {
  static const char *cases[] = {
      "HTTP/1.1 403 Forbidden\r\nContent-Length: 3\r\nContent-Length: "
      "3\r\n\r\n",
      "HTTP/1.1 403 Forbidden\r\nContent-Length: 3\r\nTransfer-Encoding: "
      "chunked\r\n\r\n",
      "HTTP/1.1 403 Forbidden\r\nContent-Length: bad\r\n\r\n",
      "HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: gzip\r\n\r\n",
      "HTTP/1.1 403 Forbidden\r\n Bad: folded\r\n\r\n",
      "HTTP/1.1 403 Forbidden\r\nContent-Length: "
      "184467440737095516160\r\n\r\n"};
  struct framer p;
  struct step step;
  unsigned char overlong[HEAD_LIMIT + 1];
  size_t i;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(&p, 0, sizeof(p));
    step = feed(&p, (const unsigned char *)cases[i], strlen(cases[i]), 64);
    assert(step.event == INVALID && p.phase == BAD);
  }
  memset(&p, 0, sizeof(p));
  step = feed(
      &p,
      (const unsigned char
           *)"HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\nx",
      sizeof("HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\nx") - 1, 64);
  assert(step.event == FINAL);
  step = feed(&p, (const unsigned char *)"x", 1, 64);
  assert(step.event == BODY && p.remaining == 1);
  assert(at_eof(&p) == INVALID);

  memset(&p, 0, sizeof(p));
  step = feed(
      &p,
      (const unsigned char
           *)"HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n",
      sizeof("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n") -
          1,
      64);
  assert(step.event == FINAL);
  step = feed(&p, (const unsigned char *)"1\r\nx!", 5, 64);
  assert(step.event == BODY && step.body_length == 1);
  step = feed(&p, (const unsigned char *)"!", 1, 64);
  assert(step.event == INVALID);

  memset(&p, 0, sizeof(p));
  step = feed(
      &p,
      (const unsigned char
           *)"HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n",
      sizeof("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n") -
          1,
      64);
  assert(step.event == FINAL);
  step = feed(&p, (const unsigned char *)"0\r\nContent-Length: 2\r\n\r\n",
              sizeof("0\r\nContent-Length: 2\r\n\r\n") - 1, 64);
  assert(step.event == INVALID);

  memset(&p, 0, sizeof(p));
  memset(overlong, 'A', sizeof(overlong));
  step = feed(&p, overlong, sizeof(overlong), 64);
  assert(step.event == INVALID && p.head_used == HEAD_LIMIT);

  memset(&p, 0, sizeof(p));
  step = feed(
      &p,
      (const unsigned char
           *)"HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n",
      sizeof("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n") -
          1,
      64);
  assert(step.event == FINAL);
  step = feed(&p, (const unsigned char *)"1\r\nx\r\n", 6, 64);
  assert(step.event == BODY);
  step = feed(&p, (const unsigned char *)"\r\n", 2, 64);
  assert(step.event == NEED_INPUT && p.phase == CHUNK_LINE);
  assert(at_eof(&p) == INVALID);
}

int main(void) {
  test_fixed();
  test_chunked();
  test_large_chunked();
  test_close_and_upgrade();
  test_invalid();
  return 0;
}
