#include "http.h"
#include "node.h"
#include "relpath.h"
#include "s3copy.h"
#include "wire.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <httplib.h>
#include <sys/stat.h>
#include <unistd.h>

HttpServer::HttpServer(Node &n) : node_(n) {}

static time_t g_start_time = 0;
static httplib::Server *g_svr = nullptr;

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool split_path(const std::string &path, std::string &bucket,
                       std::string &key) {
  if (path.empty() || path[0] != '/')
    return false;
  size_t second = path.find('/', 1);
  if (second == std::string::npos || second + 1 >= path.size())
    return false;
  bucket = path.substr(1, second - 1);
  key = path.substr(second + 1);
  return !bucket.empty() && !key.empty();
}

static bool read_fd(fd_t &fd, std::string &data) {
  struct stat st;
  if (fstat(fd.fd, &st) < 0)
    return false;
  data.resize(st.st_size);
  ssize_t n = ::read(fd.fd, data.data(), st.st_size);
  return n == (ssize_t)st.st_size;
}

static void send_raw(httplib::Response &res, std::string body) {
  res.status = 200;
  res.set_header("Content-Type", "application/octet-stream");
  res.body = std::move(body);
}

static std::string json_str(const std::string &body, const std::string &key) {
  std::string needle = "\"" + key + "\":\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  size_t end = body.find('"', pos);
  return end == std::string::npos ? "" : body.substr(pos, end - pos);
}

static std::string json_int(const std::string &body, const std::string &key) {
  std::string needle = "\"" + key + "\":";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  size_t end = pos;
  while (end < body.size() &&
         (isdigit((unsigned char)body[end]) || body[end] == '-'))
    end++;
  return body.substr(pos, end - pos);
}

// ── Handlers ─────────────────────────────────────────────────────────────────

static void handle_get(Node &node, const httplib::Request &req,
                       httplib::Response &res) {
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("invalid path"));
    return;
  }

  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res, wire_err("invalid bucket or key"));
    return;
  }
  const std::string &relpath = *relpath_opt;
  uint32_t rp_id = ring_t::compute_id(relpath);

  bool local_only = req.has_param("local");
  bool delete_after = req.has_param("delete");

  if (!local_only) {
    auto ring = node.ring.load();
    std::string dest = ring->successor(rp_id);
    if (!dest.empty() && dest != node.config.addr) {
      node.membership.hint_alive(dest);
      node.feed.put("cache.redirect", relpath.c_str(), "dest", dest.c_str());
      if (node.cache.exists(relpath)) {
        auto peers = node.membership.alive_peers();
        bool dest_alive =
            std::find(peers.begin(), peers.end(), dest) != peers.end();
        if (dest_alive) {
          time_t mt = node.lru.get_mtime(relpath);
          std::string mt_str = std::to_string(mt ? mt : time(nullptr));
          node.jobq.put("hint_pull", dest.c_str(), relpath.c_str(),
                        mt_str.c_str());
        }
      }
      node.stats.redirects++;
      send_raw(res, wire_redirect(dest));
      return;
    }
  }

  // If cache-hit ...
  if (node.cache.exists(relpath)) {
    fd_t fd;
    if (!node.cache.read(relpath, fd).ok()) {
      send_raw(res, wire_err("read error"));
      return;
    }
    if (delete_after) {
      node.cache.drop(relpath);
      node.lru.drop(relpath);
    } else {
      node.lru.touch(relpath);
    }
    node.feed.put("cache.hit", relpath.c_str());
    node.stats.hits++;
    node.stats.served++;

    struct StreamState {
      fd_t fd;
      bool done = false;
      char buf[4096];
    };
    auto st = std::make_shared<StreamState>();
    st->fd = std::move(fd);

    res.status = 200;
    res.set_chunked_content_provider(
        "application/octet-stream",
        [st](size_t, httplib::DataSink &sink) mutable -> bool {
          if (st->done)
            return true;
          ssize_t n = ::read(st->fd.fd, st->buf, sizeof(st->buf));
          if (n > 0) {
            std::string hdr = meta_data((int)n);
            if (!sink.write(hdr.data(), hdr.size()))
              return false;
            return sink.write(st->buf, (size_t)n);
          }
          if (n < 0) {
            std::string err = wire_err(strerror(errno));
            sink.write(err.data(), err.size());
            sink.done();
            st->done = true;
            return true;
          }
          std::string fin = meta_fin();
          sink.write(fin.data(), fin.size());
          sink.done();
          st->done = true;
          return true;
        });
    return;
  }

  if (local_only) {
    send_raw(res, wire_err("ERR_NOT_CACHED"));
    return;
  }

  node.stats.misses++;
  node.feed.put("cache.miss", relpath.c_str());

  node.keylock.acquire(relpath);

  // Another thread may have fetched while we waited. Open under lock so the
  // fd keeps the inode alive, then release before reading content.
  fd_t fd;
  if (node.cache.read(relpath, fd).ok()) {
    node.keylock.release(relpath);
    std::string data;
    if (!read_fd(fd, data)) {
      send_raw(res, wire_err("fetch failed"));
      return;
    }
    node.lru.touch(relpath);
    node.stats.served++;
    send_raw(res, wire_data(data.data(), data.size()));
    return;
  }

  relpath_t tmp_rel = node.cache.tmp_relpath();
  std::string tmp_abs = node.cache.abspath(tmp_rel);
  rc_t rc = s3copy_fetch(bucket, key, tmp_abs);
  if (rc.ok()) {
    rc = node.cache.move(tmp_rel, relpath);
  }
  remove(tmp_abs.c_str()); // TODO: queue this to g_jobq
  node.keylock.release(relpath);

  if (!rc.ok()) {
    node.stats.errors++;
    node.feed.put("s3copy.err", relpath.c_str(), rc.errstr());
    send_raw(res, wire_err("s3copy failed"));
    return;
  }

  node.feed.put("s3copy.ok", relpath.c_str());
  node.lru.put(relpath, time(nullptr));
  node.jobq.put("evict");

  fd_t fd2;
  std::string data;
  if (!node.cache.read(relpath, fd2).ok() || !read_fd(fd2, data)) {
    node.stats.errors++;
    send_raw(res, wire_err("read after fetch failed"));
    return;
  }
  node.stats.served++;
  send_raw(res, wire_data(data.data(), data.size()));
}

static void handle_health(Node &node, const httplib::Request &,
                          httplib::Response &res) {
  auto peers = node.membership.alive_peers();
  std::string peers_json = "[";
  for (size_t i = 0; i < peers.size(); ++i) {
    peers_json +=
        std::format("\"{}\"{}", peers[i], (i + 1 < peers.size() ? "," : ""));
  }
  peers_json += "]";

  char upsince[32] = {};
  struct tm tm_buf = {};
  gmtime_r(&g_start_time, &tm_buf);
  strftime(upsince, sizeof(upsince), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

  long diff = static_cast<long>(time(nullptr) - g_start_time);
  long d = diff / 86400;
  long h = (diff % 86400) / 3600;
  long m = (diff % 3600) / 60;
  std::string upfor = std::to_string(d) + "d " + std::to_string(h) + "h " +
                      std::to_string(m) + "m";

  auto ring = node.ring.load();
  size_t nodes = ring->node_count();

  char used_gib_buf[32];
  snprintf(used_gib_buf, sizeof(used_gib_buf), "%.2f",
           node.lru.disk_used_gib());

  res.status = 200;
  res.set_header("Content-Type", "application/json");
  res.body = std::format(
      R"({{"addr":"{}","upsince":"{}","upfor":"{}","nodes":{},"lru_size":{},"disk_pct":{},"used_gib":{},"df_error":{},"hits":{},"misses":{},"served":{},"redirects":{},"errors":{},"hint_pull":{},"peers":{}}})",
      node.config.addr, upsince, upfor, nodes, node.lru.size(),
      node.lru.disk_usepct(), used_gib_buf,
      node.lru.disk_error() ? "true" : "false", node.stats.hits.load(),
      node.stats.misses.load(), node.stats.served.load(),
      node.stats.redirects.load(), node.stats.errors.load(),
      node.stats.hint_pull.load(), peers_json);
}

static void handle_claim_keyspace(Node &node, const httplib::Request &req,
                                  httplib::Response &res) {
  std::string addr = json_str(req.body, "addr");
  std::string first_id_s = json_int(req.body, "first_id");

  if (addr.empty()) {
    send_raw(res, wire_err("missing addr"));
    return;
  }

  const char *fid = first_id_s.empty() ? "0" : first_id_s.c_str();
  node.jobq.put("claim_scan", addr.c_str(), fid);
  send_raw(res, wire_fin());
}

static void handle_hint_pull(Node &node, const httplib::Request &req,
                             httplib::Response &res) {
  std::string src = json_str(req.body, "src");
  std::string relpath = json_str(req.body, "relpath");
  std::string mtime = json_str(req.body, "mtime");

  if (src.empty() || relpath.empty()) {
    send_raw(res, wire_err("missing src or relpath"));
    return;
  }

  node.stats.hint_pull++;
  node.jobq.put("do_pull", src.c_str(), relpath.c_str(), mtime.c_str());
  send_raw(res, wire_fin());
}

static void handle_put(Node &node, const httplib::Request &req,
                       httplib::Response &res,
                       const httplib::ContentReader &content_reader) {
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("invalid path"));
    return;
  }
  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res, wire_err("invalid bucket or key"));
    return;
  }
  const std::string &relpath = *relpath_opt;

  fd_t fd;
  if (!node.cache.write(relpath, fd).ok()) {
    send_raw(res, wire_err("write failed"));
    return;
  }

  bool ok = true;
  bool read_success = content_reader([&](const char *data, size_t data_length) {
    size_t written = 0;
    while (written < data_length) {
      ssize_t n = ::write(fd.fd, data + written, data_length - written);
      if (n < 0) {
        ok = false;
        return false;
      }
      written += n;
    }
    return true;
  });
  fd.close();

  if (!ok || !read_success) {
    node.cache.drop(relpath);
    send_raw(res, wire_err("write failed"));
    return;
  }
  node.lru.put(relpath, time(nullptr));
  node.jobq.put("evict");
  send_raw(res, wire_fin());
}

static void handle_delete(Node &node, const httplib::Request &req,
                          httplib::Response &res) {
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("invalid path"));
    return;
  }
  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res, wire_err("invalid bucket or key"));
    return;
  }
  const std::string &relpath = *relpath_opt;
  node.cache.drop(relpath);
  node.lru.drop(relpath);
  send_raw(res, wire_fin());
}

// ── Server lifecycle
// ──────────────────────────────────────────────────────────

void HttpServer::start(const std::string &host, int port) {
  g_start_time = time(nullptr);
  auto *svr = new httplib::Server();
  g_svr = svr;

  svr->Get("/_health",
           [this](const httplib::Request &req, httplib::Response &res) {
             handle_health(node_, req, res);
           });
  svr->Post("/_claim_keyspace",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_claim_keyspace(node_, req, res);
            });
  svr->Post("/_hint_pull",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_hint_pull(node_, req, res);
            });
  svr->Get(R"(/[^/]+/.+)",
           [this](const httplib::Request &req, httplib::Response &res) {
             handle_get(node_, req, res);
           });
  svr->Put(R"(/[^/]+/.+)",
           [this](const httplib::Request &req, httplib::Response &res,
                  const httplib::ContentReader &content_reader) {
             handle_put(node_, req, res, content_reader);
           });
  svr->Delete(R"(/[^/]+/.+)",
              [this](const httplib::Request &req, httplib::Response &res) {
                handle_delete(node_, req, res);
              });

  svr->listen(host, port);
}

void HttpServer::stop() {
  if (g_svr)
    g_svr->stop();
}
