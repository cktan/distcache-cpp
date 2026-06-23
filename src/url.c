#include "url.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} growbuf_t;

/* libcurl write callback: append bytes to growbuf, resizing as needed. */
static size_t growbuf_write_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  growbuf_t *buf = ud;
  size_t n = sz * nmemb;
  if (buf->len + n > buf->cap) {
    size_t new_cap = (buf->len + n) * 2;
    if (new_cap < 8192)
      new_cap = 8192;
    char *tmp = realloc(buf->data, new_cap);
    if (!tmp)
      return 0;
    buf->data = tmp;
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->len, ptr, n);
  buf->len += n;
  return n;
}

/* Header callback: pre-allocate growbuf from Content-Length if present. */
static size_t content_length_header_cb(char *buf, size_t sz, size_t n,
                                       void *ud) {
  size_t len = sz * n;
  if (len > 16 && strncmp(buf, "Content-Length:", 15) == 0) {
    growbuf_t *gb = ud;
    long cl = atol(buf + 15);
    if (cl > 0 && gb->data == NULL) {
      gb->data = malloc((size_t)cl + 1);
      if (gb->data)
        gb->cap = (size_t)cl + 1;
    }
  }
  return len;
}

/* See url.h for contract. */
int url_get(const char *url, int *ret_status, int *ret_size, char **ret_body) {
  *ret_status = 0;
  *ret_size = 0;
  *ret_body = NULL;

  CURL *h = curl_easy_init();
  if (!h)
    return -1;

  growbuf_t body = {0};
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, content_length_header_cb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &body);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, growbuf_write_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  CURLcode crc = curl_easy_perform(h);
  long http_code = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(h);

  if (crc != CURLE_OK) {
    free(body.data);
    return -1;
  }

  *ret_status = (int)http_code;

  if (http_code < 200 || http_code >= 300) {
    free(body.data);
    *ret_size = 0;
    return -1;
  }

  /* NUL-terminate. */
  if (body.cap == body.len) {
    char *tmp = realloc(body.data, body.len + 1);
    if (!tmp) {
      free(body.data);
      *ret_status = 0;
      return -1;
    }
    body.data = tmp;
  }
  body.data[body.len] = '\0';

  *ret_size = (int)body.len;
  *ret_body = body.data;
  return 0;
}
