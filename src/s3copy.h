#pragma once
#include <string>

// Fetch bucket/key from S3 via the s3copy sidecar.
// Sends POST to $S3COPY_URL (default http://localhost:7777/copy) with
// JSON body {"bucket":…,"key":…,"dest":…}. The sidecar writes the object
// atomically to dest_abspath.
// Returns true on HTTP 200, false otherwise.
bool s3copy_fetch(const std::string &bucket, const std::string &key,
                  const std::string &dest_abspath);
