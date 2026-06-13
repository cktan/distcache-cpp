#include "s3copy.h"
#include "cache.h"
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

bool s3copy_fetch(const std::string &bucket, const std::string &key,
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

  // Build JSON body manually — no JSON library dependency.
  auto escape = [](const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '"' || c == '\\')
        out += '\\';
      out += c;
    }
    return out;
  };

  std::string tmp = tmp_path();
  std::string body = "{\"bucket\":\"" + escape(bucket) + "\",\"key\":\"" +
                     escape(key) + "\",\"dest\":\"" + escape(tmp) + "\"}";

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(60);

  auto res = cli.Post(base_path + "/copy", body, "application/json");
  if (!res || res->status != 200) {
    remove(tmp.c_str());
    return false;
  }
  if (!cache_mkdirs(dest_abspath) ||
      rename(tmp.c_str(), dest_abspath.c_str()) != 0) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}
