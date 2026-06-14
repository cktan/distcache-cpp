#include "s3copy.h"
#include "httplib.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Parse "http://host:port" into host and port.
static bool parse_url(const std::string &url, std::string &host, int &port,
                      std::string &base_path) {
  const char *p = url.c_str();
  if (strncmp(p, "http://", 7) != 0)
    return false;
  p += 7;

  const char *colon = strchr(p, ':');
  const char *slash = strchr(p, '/');

  if (colon && (!slash || colon < slash)) {
    host = std::string(p, colon - p);
    port = atoi(colon + 1);
    base_path = slash ? std::string(slash) : "/";
  } else if (slash) {
    host = std::string(p, slash - p);
    port = 80;
    base_path = std::string(slash);
  } else {
    host = std::string(p);
    port = 80;
    base_path = "/";
  }
  return !host.empty() && port > 0;
}

rc_t s3copy_fetch(const std::string &bucket, const std::string &key,
                  const std::string &dest_abspath) {
  const char *env_url = getenv("S3COPY_URL");
  std::string url = env_url ? env_url : "http://localhost:7777";

  std::string host, base_path;
  int port = 7777;
  if (!parse_url(url, host, port, base_path)) {
    host = "localhost";
    port = 7777;
    base_path = "";
  }

  auto escape = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '"' || c == '\\')
        out += '\\';
      out += c;
    }
    return out;
  };

  std::string body = "{\"bucket\":\"" + escape(bucket) + "\",\"key\":\"" +
                     escape(key) + "\",\"dest\":\"" + escape(dest_abspath) +
                     "\"}";

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(60);

  auto res = cli.Post(base_path + "/copy", body, "application/json");
  if (!res)
    return rc_t::err(rc_errcode_t::s3_error, "connection failed");
  if (res->status == 404)
    return rc_t::err(rc_errcode_t::s3_not_found, "not found");
  if (res->status == 403 || res->status == 401)
    return rc_t::err(rc_errcode_t::s3_forbidden, "forbidden");
  if (res->status != 200)
    return rc_t::err(rc_errcode_t::s3_error,
                     "HTTP " + std::to_string(res->status));
  return rc_t::ok();
}
