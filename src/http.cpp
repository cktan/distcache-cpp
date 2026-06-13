#include "http.h"
#include "cache.h"
#include "config.h"
#include "feed.h"
#include "httplib.h"
#include "keylock.h"
#include "lru.h"
#include "membership.h"
#include "rebalance.h"
#include "relpath.h"
#include "ring.h"
#include "s3copy.h"
#include "wire.h"
#include <algorithm>
#include <ctime>
#include <functional>

HttpServer g_http;

// ── Helpers ──────────────────────────────────────────────────────────────────

// Split "/bucket/key/with/slashes" into {bucket, key}.
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

static void send_raw(httplib::Response &res, std::string body) {
  res.status = 200;
  res.set_header("Content-Type", "application/octet-stream");
  res.body = std::move(body);
}

// Minimal JSON string extraction: find "key":"value" → value. Returns "" if
// absent.
static std::string json_str(const std::string &body, const std::string &key) {
  std::string needle = "\"" + key + "\":\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  size_t end = body.find('"', pos);
  return end == std::string::npos ? "" : body.substr(pos, end - pos);
}

// Minimal JSON integer extraction: find "key":N → N as string. Returns "" if
// absent.
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

// ── GET handler
// ───────────────────────────────────────────────────────────────

static void handle_get(const httplib::Request &req, httplib::Response &res) {
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
  uint32_t rp_id = Ring::compute_relpath_id(relpath);
  std::string abspath = to_abs(g_config.homedir, relpath);

  bool local_only = req.has_param("local");
  bool delete_after = req.has_param("delete");

  // Ring lookup — skip redirect if ?local=1.
  if (!local_only) {
    auto ring = g_ring.load();
    std::string dest = ring->successor(rp_id);
    if (!dest.empty() && dest != g_config.addr) {
      g_membership.hint_alive(dest);
      g_feed.put(4, "cache.redirect", relpath.c_str(), "dest", dest.c_str());
      // On-hit hint: if we hold the file locally, tell dest to pull it.
      if (cache_exists(abspath)) {
        auto peers = g_membership.alive_peers();
        bool dest_alive =
            std::find(peers.begin(), peers.end(), dest) != peers.end();
        if (dest_alive) {
          time_t mt = g_lru.get_mtime(relpath);
          std::string mt_str = std::to_string(mt ? mt : time(nullptr));
          g_jobq.put(4, "hint_pull", dest.c_str(), relpath.c_str(),
                     mt_str.c_str());
        }
      }
      send_raw(res, wire_redirect(dest));
      return;
    }
  }

  // Serve from local cache.
  if (cache_exists(abspath)) {
    std::string data;
    if (!cache_read(abspath, data)) {
      send_raw(res, wire_err("read error"));
      return;
    }
    if (delete_after) {
      cache_del(abspath);
      g_lru.remove(relpath);
    } else {
      g_lru.touch(relpath);
    }
    g_feed.put(2, "cache.hit", relpath.c_str());
    send_raw(res, wire_data(data.data(), data.size()));
    return;
  }

  // Local-only miss: return ERR_NOT_CACHED (used by inter-node pull).
  if (local_only) {
    send_raw(res, wire_err("ERR_NOT_CACHED"));
    return;
  }

  // Cache miss on owned key: fetch from S3 via sidecar.
  g_feed.put(2, "cache.miss", relpath.c_str());

  bool is_owner = g_keylock.acquire(relpath);
  if (!is_owner) {
    // Another thread fetched while we waited; re-check cache.
    if (cache_exists(abspath)) {
      std::string data;
      if (cache_read(abspath, data)) {
        g_lru.touch(relpath);
        send_raw(res, wire_data(data.data(), data.size()));
        return;
      }
    }
    send_raw(res, wire_err("fetch failed"));
    return;
  }

  // We are the owner: drive the S3 fetch.
  bool ok = s3copy_fetch(bucket, key, abspath);
  g_keylock.release(relpath);

  if (!ok) {
    g_feed.put(3, "s3copy.err", relpath.c_str(), "fetch failed");
    send_raw(res, wire_err("s3copy failed"));
    return;
  }

  g_feed.put(2, "s3copy.ok", relpath.c_str());
  g_lru.insert(relpath, time(nullptr), rp_id);
  g_jobq.put(1, "evict");

  std::string data;
  if (!cache_read(abspath, data)) {
    send_raw(res, wire_err("read after fetch failed"));
    return;
  }
  send_raw(res, wire_data(data.data(), data.size()));
}

// ── POST /_claim_keyspace
// ─────────────────────────────────────────────────────

static void handle_claim_keyspace(const httplib::Request &req,
                                  httplib::Response &res) {
  std::string addr = json_str(req.body, "addr");
  std::string first_id_s = json_int(req.body, "first_id");

  if (addr.empty()) {
    send_raw(res, wire_err("missing addr"));
    return;
  }

  // Offload inject + scan + enqueue to g_jobq worker.
  const char *fid = first_id_s.empty() ? "0" : first_id_s.c_str();
  g_jobq.put(3, "claim_scan", addr.c_str(), fid);
  send_raw(res, wire_fin());
}

// ── POST /_hint_pull
// ──────────────────────────────────────────────────────────

static void handle_hint_pull(const httplib::Request &req,
                             httplib::Response &res) {
  std::string src = json_str(req.body, "src");
  std::string relpath = json_str(req.body, "relpath");
  std::string mtime = json_str(req.body, "mtime");

  if (src.empty() || relpath.empty()) {
    send_raw(res, wire_err("missing src or relpath"));
    return;
  }

  // Offload pull to g_jobq worker; return immediately.
  g_jobq.put(4, "do_pull", src.c_str(), relpath.c_str(), mtime.c_str());
  send_raw(res, wire_fin());
}

// ── PUT handler
// ───────────────────────────────────────────────────────────────

static void handle_put(const httplib::Request &req, httplib::Response &res) {
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
  std::string abspath = to_abs(g_config.homedir, relpath);
  uint32_t rp_id = Ring::compute_relpath_id(relpath);

  if (!cache_write(abspath, req.body.data(), req.body.size())) {
    send_raw(res, wire_err("write failed"));
    return;
  }
  g_lru.insert(relpath, time(nullptr), rp_id);
  g_jobq.put(1, "evict");
  send_raw(res, wire_fin());
}

// ── DELETE handler
// ────────────────────────────────────────────────────────────

static void handle_delete(const httplib::Request &req, httplib::Response &res) {
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
  cache_del(to_abs(g_config.homedir, relpath));
  g_lru.remove(relpath);
  send_raw(res, wire_fin());
}

// ── Server lifecycle
// ──────────────────────────────────────────────────────────

static httplib::Server *g_svr = nullptr;

void HttpServer::start(const std::string &host, int port) {
  auto *svr = new httplib::Server();
  g_svr = svr;

  svr->Post("/_claim_keyspace", handle_claim_keyspace);
  svr->Post("/_hint_pull", handle_hint_pull);
  svr->Get(R"(/[^/]+/.+)", handle_get);
  svr->Put(R"(/[^/]+/.+)", handle_put);
  svr->Delete(R"(/[^/]+/.+)", handle_delete);

  svr->listen(host, port);
}

void HttpServer::stop() {
  if (g_svr)
    g_svr->stop();
}
