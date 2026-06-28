#include "codec.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Write the JSON-escaped form of src into buf[0..bufsz-1] (NUL-terminated).
 * Returns buf on success, NULL if buf is too small. */
char *json_encode_str(const char *src, char *buf, int bufsz) {
  char *p = buf;
  char *end = buf + bufsz - 1; /* reserve one byte for NUL */
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int need;
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t')
      need = 2;
    else if (c < 0x20)
      need = 6;
    else
      need = 1;
    if (p + need > end)
      return NULL;
    if (c == '"')       { *p++ = '\\'; *p++ = '"';  }
    else if (c == '\\') { *p++ = '\\'; *p++ = '\\'; }
    else if (c == '\n') { *p++ = '\\'; *p++ = 'n';  }
    else if (c == '\r') { *p++ = '\\'; *p++ = 'r';  }
    else if (c == '\t') { *p++ = '\\'; *p++ = 't';  }
    else if (c < 0x20)  { p += sprintf(p, "\\u%04x", c); }
    else                { *p++ = (char)c; }
  }
  *p = '\0';
  return buf;
}

/* Write the percent-encoded form of src into buf[0..bufsz-1] (NUL-terminated).
 * Returns buf on success, NULL if buf is too small. */
char *url_encode(const char *src, char *buf, int bufsz) {
  static const char hex[] = "0123456789ABCDEF";
  char *p = buf;
  char *end = buf + bufsz - 1;
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      if (p >= end)
        return NULL;
      *p++ = (char)c;
    } else {
      if (p + 3 > end)
        return NULL;
      *p++ = '%';
      *p++ = hex[c >> 4];
      *p++ = hex[c & 0xf];
    }
  }
  *p = '\0';
  return buf;
}

/* Decode percent-encoded sequences in src into buf[0..bufsz-1] (NUL-terminated).
 * Returns buf on success, NULL if buf is too small. */
char *url_decode(const char *src, char *buf, int bufsz) {
  char *p = buf;
  char *end = buf + bufsz - 1;
  for (const char *s = src; *s; ) {
    if (p >= end)
      return NULL;
    if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char hi = s[1], lo = s[2];
      int v = (isdigit((unsigned char)hi) ? hi - '0' : toupper((unsigned char)hi) - 'A' + 10) << 4 |
              (isdigit((unsigned char)lo) ? lo - '0' : toupper((unsigned char)lo) - 'A' + 10);
      *p++ = (char)v;
      s += 3;
    } else {
      *p++ = *s++;
    }
  }
  *p = '\0';
  return buf;
}
