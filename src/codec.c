#include "codec.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* JSON-escape src into buf[0..bufsz-1].  Returns N, the number of bytes
 * required including NUL.  Writes to buf only when N <= bufsz. */
int json_encode_str(const char *src, char *buf, int bufsz) {
  int n = 1;
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t')
      n += 2;
    else if (c < 0x20)
      n += 6;
    else
      n++;
  }
  if (n > bufsz)
    return n;
  char *p = buf;
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"') {
      *p++ = '\\';
      *p++ = '"';
    } else if (c == '\\') {
      *p++ = '\\';
      *p++ = '\\';
    } else if (c == '\n') {
      *p++ = '\\';
      *p++ = 'n';
    } else if (c == '\r') {
      *p++ = '\\';
      *p++ = 'r';
    } else if (c == '\t') {
      *p++ = '\\';
      *p++ = 't';
    } else if (c < 0x20) {
      p += sprintf(p, "\\u%04x", c);
    } else {
      *p++ = (char)c;
    }
  }
  *p = '\0';
  return n;
}

/* Percent-encode src into buf[0..bufsz-1].  Returns N, the number of bytes
 * required including NUL.  Writes to buf only when N <= bufsz. */
int url_encode(const char *src, char *buf, int bufsz) {
  static const char hex[] = "0123456789ABCDEF";
  int n = 1;
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~' || c == '/';
    n += unres ? 1 : 3;
  }
  if (n > bufsz)
    return n;
  char *p = buf;
  for (const char *s = src; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~' || c == '/';
    if (unres) {
      *p++ = (char)c;
    } else {
      *p++ = '%';
      *p++ = hex[c >> 4];
      *p++ = hex[c & 0xf];
    }
  }
  *p = '\0';
  return n;
}

/* Decode percent-encoded sequences in src in-place.  Returns src. */
char *url_decode(char *src) {
  char *r = src, *w = src;
  while (*r) {
    if (r[0] == '%' && isxdigit((unsigned char)r[1]) &&
        isxdigit((unsigned char)r[2])) {
      int hi = isdigit((unsigned char)r[1])
                   ? r[1] - '0'
                   : toupper((unsigned char)r[1]) - 'A' + 10;
      int lo = isdigit((unsigned char)r[2])
                   ? r[2] - '0'
                   : toupper((unsigned char)r[2]) - 'A' + 10;
      *w++ = (char)((hi << 4) | lo);
      r += 3;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
  return src;
}
