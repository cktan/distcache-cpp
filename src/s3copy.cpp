#include "s3copy.h"
#include "codec.h"
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

// Extract a string or boolean value from a flat JSON object by key.
// Returns the unquoted string value, or the raw token for booleans/numbers.
// Returns "" if the key is absent or the body is malformed.
static std::string json_field(const std::string &body, const char *key) {
  std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':'))
    pos++;
  if (pos >= body.size())
    return "";
  if (body[pos] == '"') {
    // String value: scan to closing quote, honouring backslash escapes.
    pos++;
    std::string val;
    while (pos < body.size() && body[pos] != '"') {
      if (body[pos] == '\\' && pos + 1 < body.size())
        val += body[++pos];
      else
        val += body[pos];
      pos++;
    }
    return val;
  }
  // Boolean or number: scan to next delimiter.
  size_t end = body.find_first_of(",}\n", pos);
  return body.substr(pos,
                     end == std::string::npos ? std::string::npos : end - pos);
}

// Map the sidecar's errtype string to a C++ error code.
static rc_errcode_t errtype_to_rc(const std::string &errtype) {
  if (errtype == "ERR_NOT_FOUND")
    return rc_errcode_t::ERR_S3COPY_NOT_FOUND;
  if (errtype == "ERR_UNAUTHORIZED")
    return rc_errcode_t::ERR_S3COPY_UNAUTHORIZED;
  if (errtype == "ERR_BAD_REQUEST")
    return rc_errcode_t::ERR_S3COPY_BAD_REQUEST;
  if (errtype == "ERR_S3_ERROR")
    return rc_errcode_t::ERR_S3COPY_S3_ERROR;
  if (errtype == "ERR_NETWORK")
    return rc_errcode_t::ERR_S3COPY_NETWORK;
  if (errtype == "ERR_IO")
    return rc_errcode_t::ERR_S3COPY_IO;
  return rc_errcode_t::ERR_S3_COPY;
}

// Ask the s3copy sidecar to download s3://bucket/key and write it to
// dest_abspath on the local filesystem.
rc_t s3copy_fetch(const std::string &bucket, const std::string &key,
                  const std::string &dest_abspath) {
  // Build the JSON request body: {"bucket":…,"key":…,"dest":…}
  std::string body = std::string("{\"bucket\":\"") + json_encode_str(bucket) +
                     "\",\"key\":\"" + json_encode_str(key) + "\",\"dest\":\"" +
                     json_encode_str(dest_abspath) + "\"}";

  // POST to sidecar and wait up to 60 s for the S3 transfer to complete.
  httplib::Client cli(g_host, g_port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(60);

  auto res = cli.Post(g_base_path + "/copy", body, "application/json");
  if (!res)
    return rc_t::err(rc_errcode_t::ERR_S3_COPY, "connection failed");
  if (res->status == 404)
    return rc_t::err(rc_errcode_t::ERR_S3COPY_HTTP_404,
                     "sidecar endpoint not found");
  if (res->status != 200)
    return rc_t::err(rc_errcode_t::ERR_S3COPY_HTTP_ERR,
                     "HTTP " + std::to_string(res->status));
  // Parse the JSON body: {"ok": true} or {"ok": false, "errtype": "...",
  // "errmsg": "..."}
  std::string ok = json_field(res->body, "ok");
  if (ok == "true")
    return {};
  if (ok != "false")
    return rc_t::err(rc_errcode_t::ERR_S3_COPY, "invalid sidecar response");
  std::string errtype = json_field(res->body, "errtype");
  std::string errmsg = json_field(res->body, "errmsg");
  return rc_t::err(errtype_to_rc(errtype),
                   errmsg.empty() ? "sidecar error" : errmsg);
}
