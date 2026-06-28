#include "s3copy.h"
#include "httplib.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::string g_host = "localhost";
static int g_port = 7777;
static std::string g_base_path = "/";

// Parse "http://host:port/base" into components.
// base_path captures any path prefix after the host:port so that the sidecar
// can be deployed behind a reverse proxy at a non-root path.
// Falls back to port 80 when no port is specified.
static bool parse_url(const std::string &url, std::string &host, int &port,
                      std::string &base_path) {
  const char *p = url.c_str();
  if (strncmp(p, "http://", 7) != 0)
    return false;
  p += 7;

  const char *colon = strchr(p, ':');
  const char *slash = strchr(p, '/');

  if (colon && (!slash || colon < slash)) {
    // host:port[/base]
    host = std::string(p, colon - p);
    port = atoi(colon + 1);
    base_path = slash ? std::string(slash) : "/";
  } else if (slash) {
    // host/base  (no explicit port)
    host = std::string(p, slash - p);
    port = 80;
    base_path = std::string(slash);
  } else {
    // bare host only
    host = std::string(p);
    port = 80;
    base_path = "/";
  }
  return !host.empty() && port > 0;
}

// Initialize the s3copy module from $S3COPY_URL.
void s3copy_init(void) {
  const char *env = getenv("S3COPY_URL");
  std::string url = env ? env : "http://localhost:7777";
  if (!parse_url(url, g_host, g_port, g_base_path)) {
    g_host = "localhost";
    g_port = 7777;
    g_base_path = "/";
  }
}

// Probe the sidecar; return true if GET /ping responds HTTP 200.
bool s3copy_ping(void) {
  httplib::Client cli(g_host, g_port);
  cli.set_connection_timeout(1);
  auto res = cli.Get(g_base_path + "/ping");
  return res && res->status == 200;
}

// Ask the s3copy sidecar to download s3://bucket/key and write it to
// dest_abspath on the local filesystem.
rc_t s3copy_fetch(const std::string &bucket, const std::string &key,
                  const std::string &dest_abspath) {
  // Escape " and \ so bucket/key/path are safe to embed in a JSON string.
  auto escape = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '"' || c == '\\')
        out += '\\';
      out += c;
    }
    return out;
  };

  // Build the JSON request body: {"bucket":…,"key":…,"dest":…}
  std::string body = "{\"bucket\":\"" + escape(bucket) + "\",\"key\":\"" +
                     escape(key) + "\",\"dest\":\"" + escape(dest_abspath) +
                     "\"}";

  // POST to sidecar and wait up to 60 s for the S3 transfer to complete.
  httplib::Client cli(g_host, g_port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(60);

  auto res = cli.Post(g_base_path + "/copy", body, "application/json");
  if (!res)
    return rc_t::err(rc_errcode_t::s3_error, "connection failed");
  if (res->status == 404)
    return rc_t::err(rc_errcode_t::s3_not_found, "not found");
  if (res->status == 403 || res->status == 401)
    return rc_t::err(rc_errcode_t::s3_forbidden, "forbidden");
  if (res->status != 200)
    return rc_t::err(rc_errcode_t::s3_error,
                     "HTTP " + std::to_string(res->status));
  return {};
}
