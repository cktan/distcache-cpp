#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Perform a one-shot GET request to url.
 *
 * Returns 0 on 2xx: *ret_status = HTTP code, *ret_size = body bytes
 * (excl. NUL), *ret_body = NUL-terminated body (caller must free).
 *
 * Returns -1 on transport error or non-2xx: *ret_status = HTTP code
 * if available else 0, *ret_size = 0, *ret_body = NULL.
 */
int url_get(const char *url, int *ret_status, int *ret_size, char **ret_body);

#ifdef __cplusplus
}
#endif
