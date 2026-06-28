#pragma once
#include "rc.h"
#include <string>

// Initialize s3copy from $S3COPY_URL (default http://localhost:7777).
// Must be called once before s3copy_ping() or s3copy_fetch().
void s3copy_init(void);

// Return true if the s3copy sidecar answers GET /ping with HTTP 200.
bool s3copy_ping(void);

// Fetch bucket/key from S3 via the s3copy sidecar.
// Sends POST to $S3COPY_URL/copy with JSON body {"bucket":…,"key":…,"dest":…}.
// The sidecar writes the object to dest_abspath.
// Caller is responsible for atomic placement (move).
rc_t s3copy_fetch(const std::string &bucket, const std::string &key,
                  const std::string &dest_abspath);
