#include "json.h"
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
