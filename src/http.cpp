#define JSMN_IMPLEMENTATION
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
#include <jsmn.h>
#include <unistd.h>
#include <vector>

// Bind server to node_.
http_server_t::http_server_t(node_t &n) : node_(n) {}

// Default — httplib::Server cleanup is handled by unique_ptr.
http_server_t::~http_server_t() = default;

// ── Helpers ──────────────────────────────────────────────────────────────────

// Parse /bucket/key from path; return false if missing or malformed.
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

// Send a 200 octet-stream response with body.
static void send_raw(httplib::Response &res, std::string body) {
  res.status = 200;
  res.set_header("Content-Type", "application/octet-stream");
  res.body = std::move(body);
}

// Extract the string value of key from a flat JSON object body; return ""
// if not found or malformed.
static std::string json_str(const std::string &body, const std::string &key) {
  jsmn_parser p;
  jsmn_init(&p);
  int n = jsmn_parse(&p, body.c_str(), body.size(), nullptr, 0);
  if (n < 0)
    return "";
  std::vector<jsmntok_t> toks(n);
  jsmn_init(&p);
  if (jsmn_parse(&p, body.c_str(), body.size(), toks.data(), n) < 0)
    return "";
  // toks[0] is the root object; pairs start at index 1
  for (int i = 1; i + 1 < n; i += 2) {
    if (toks[i].type != JSMN_STRING)
      break;
    std::string k(body.c_str() + toks[i].start, toks[i].end - toks[i].start);
    if (k == key && toks[i + 1].type == JSMN_STRING)
      return std::string(body.c_str() + toks[i + 1].start,
                         toks[i + 1].end - toks[i + 1].start);
  }
  return "";
}

// writer_fn adapter that writes into a chunked httplib DataSink.
static int sink_writer(void *ctx, const char *buf, size_t n) {
  return static_cast<httplib::DataSink *>(ctx)->write(buf, n) ? 0 : -1;
}

// Read one chunk from fd and emit it as a DATA frame into sink.
// Sends FIN on EOF or error and sets done=true.
static bool send_file(fd_t &fd, bool &done, httplib::DataSink &sink) {
  char buf[4096];
  ssize_t n = ::read(fd.fd, buf, sizeof(buf));
  if (n > 0) {
    // got a chunk
    return stream_out(sink_writer, &sink, FRAME_DATA, buf, (size_t)n) == 0;
  }
  if (n < 0) {
    // got error
    std::string err = wire_err(strerror(errno));
    sink.write(err.data(), err.size());
  }
  // EOF - normal case - write a FIN frame
  stream_out(sink_writer, &sink, FRAME_FIN, nullptr, 0);
  sink.done();
  done = true;
  return true;
}

// Set up a chunked 200 response that streams fd as framed DATA/FIN chunks.
static void serve_fd(fd_t &fd, httplib::Response &res) {
  struct StreamState {
    fd_t fd;
    bool done = false;
  };
  auto st = std::make_shared<StreamState>();
  st->fd = std::move(fd);
  res.status = 200;
  res.set_chunked_content_provider(
      "application/octet-stream",
      [st](size_t, httplib::DataSink &sink) mutable -> bool {
        if (st->done)
          return true;
        return send_file(st->fd, st->done, sink);
      });
}

// ── Handlers ─────────────────────────────────────────────────────────────────

// GET /{bucket}/{key}: redirect to owner, serve from cache, or fetch from S3.
static void handle_get(node_t &node, const httplib::Request &req,
                       httplib::Response &res) {
  // Obtain bucket & key, form relpath, and compute relpath_id.
  // We need the relpath_id to interrogate ring for the responsible node.
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
  const relpath_t &relpath = *relpath_opt;
  uint32_t rp_id = ring_t::compute_id(relpath.str());

  // Check for flags
  bool local_only = req.has_param("local");
  bool no_redirect = req.has_param("no_redirect");
  bool delete_after = req.has_param("delete");

  if (!local_only && !no_redirect) {
    // The normal case! We need to redirect if this node is not dest.

    auto ring = node.ring.load();
    std::string dest = ring->successor(rp_id);
    if (!dest.empty() && dest != node.nodeid) {
      // Key belongs to another ring node; update membership and redirect.
      std::string dest_addr = membership_t::parse_peer(dest).first;
      node.membership.hint_alive(dest_addr);
      node.feed.put("cache.redirect", relpath.str().c_str(), "dest",
                    dest.c_str());

      fd_t fd;
      if (node.cache.read(relpath, fd).ok()) {
        // We have the file; serve it and let dest pull it from us.
        if (delete_after) {
          node.cache.drop(relpath);
        }
        time_t mt = node.cache.get_mtime(relpath.str());
        std::string mt_str = std::to_string(mt ? mt : time(nullptr));
        node.jobq.put("hint_pull", dest_addr.c_str(), relpath.str().c_str(),
                      mt_str.c_str());
        node.feed.put("cache.hit", relpath.str().c_str());
        node.stats.hits++;
        node.stats.served++;
        serve_fd(fd, res);
        return;
      }
      node.stats.redirects++;
      send_raw(res, wire_redirect(dest));
      return;
    }
  }

  // This node is dest (or is forced to be dest).

  fd_t fd;
  if (node.cache.read(relpath, fd).ok()) {
    // Cache hit: open fd before optional drop so inode stays live after unlink.
    if (delete_after) {
      node.cache.drop(relpath);
    }
    node.feed.put("cache.hit", relpath.str().c_str());
    node.stats.hits++;
    node.stats.served++;
    serve_fd(fd, res);
    return;
  }

  // Cache miss:
  node.stats.misses++;
  node.feed.put("cache.miss", relpath.str().c_str());

  // If no download from S3, then done.
  if (local_only) {
    send_raw(res, wire_err("ERR_NOT_CACHED"));
    return;
  }

  // Grab keylock before attempting to download from S3
  keylock_guard_t kl_guard(node.keylock, relpath.str());

  // Another thread may have fetched while we waited. Open under lock so the
  // fd keeps the inode alive, then release the lock before reading content.
  {
    fd_t fd;
    if (node.cache.read(relpath, fd).ok()) {
      kl_guard.release();
      node.stats.served++;
      serve_fd(fd, res);
      return;
    }
  }

  // Set up to download from S3 into a temp file
  relpath_t tmp_rel = node.cache.tmp_relpath();
  std::string tmp_abs = node.cache.abspath(tmp_rel);
  rc_t rc = s3copy_fetch(bucket, key, tmp_abs);
  if (rc.ok()) {
    rc = node.cache.move(tmp_rel, relpath);
  }
  if (!rc.ok()) {
    remove(tmp_abs.c_str());
  }
  kl_guard.release();

  if (!rc.ok()) {
    node.stats.errors++;
    node.feed.put("s3copy.err", relpath.str().c_str(), rc.errstr());
    send_raw(res, wire_err("s3copy failed"));
    return;
  }

  node.feed.put("s3copy.ok", relpath.str().c_str());
  node.jobq.put("evict");

  fd_t fd2;
  if (!node.cache.read(relpath, fd2).ok()) {
    node.stats.errors++;
    send_raw(res, wire_err("read after fetch failed"));
    return;
  }
  node.stats.served++;
  serve_fd(fd2, res);
}

// GET /_health: return JSON with node stats, uptime, ring size, and peers.
static void handle_health(node_t &node, const httplib::Request &,
                          httplib::Response &res, time_t start_time) {
  auto peers = node.membership.alive_peers();
  std::string peers_json = "[";
  for (size_t i = 0; i < peers.size(); ++i) {
    peers_json +=
        std::format("\"{}\"{}", peers[i], (i + 1 < peers.size() ? "," : ""));
  }
  peers_json += "]";

  char upsince[32] = {};
  struct tm tm_buf = {};
  gmtime_r(&start_time, &tm_buf);
  strftime(upsince, sizeof(upsince), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

  long diff = static_cast<long>(time(nullptr) - start_time);
  long d = diff / 86400;
  long h = (diff % 86400) / 3600;
  long m = (diff % 3600) / 60;
  std::string upfor = std::to_string(d) + "d " + std::to_string(h) + "h " +
                      std::to_string(m) + "m";

  auto ring = node.ring.load();
  size_t nodes = ring->node_count();

  char used_gib_buf[32];
  snprintf(used_gib_buf, sizeof(used_gib_buf), "%.2f",
           node.cache.disk_used_gib());

  res.status = 200;
  res.set_header("Content-Type", "application/json");
  res.body = std::format(
      R"({{"addr":"{}","upsince":"{}","upfor":"{}","nodes":{},"lru_size":{},"disk_pct":{},"used_gib":{},"df_error":{},"hits":{},"misses":{},"served":{},"redirects":{},"errors":{},"hint_pull":{},"peers":{}}})",
      node.nodeid, upsince, upfor, nodes, node.cache.size(),
      node.cache.disk_usepct(), used_gib_buf,
      node.cache.disk_error() ? "true" : "false", node.stats.hits.load(),
      node.stats.misses.load(), node.stats.served.load(),
      node.stats.redirects.load(), node.stats.errors.load(),
      node.stats.hint_pull.load(), peers_json);
}

// GET /_nodes: return JSON array of all known ring members.
static void handle_nodes(node_t &node, const httplib::Request &,
                         httplib::Response &res) {
  auto peers = node.membership.raw_peers();
  std::string json = "[";
  for (size_t i = 0; i < peers.size(); ++i) {
    if (i > 0)
      json += ",";
    json += "\"" + peers[i] + "\"";
  }
  json += "]";
  send_raw(res, wire_info(json));
}

// POST /_claim_keyspace: enqueue claim_scan for a joining node's keyspace.
static void handle_claim_keyspace(node_t &node, const httplib::Request &req,
                                  httplib::Response &res) {
  std::string nodeid = json_str(req.body, "nodeid");

  if (nodeid.empty()) {
    send_raw(res, wire_err("missing nodeid"));
    return;
  }

  node.jobq.put("claim_scan", nodeid.c_str());
  send_raw(res, wire_fin());
}

// POST /_hint_pull: enqueue do_pull so this node fetches relpath from src.
static void handle_hint_pull(node_t &node, const httplib::Request &req,
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

// PUT /{bucket}/{key}: stream request body into cache and trigger eviction.
static void handle_put(node_t &node, const httplib::Request &req,
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
  const relpath_t &relpath = *relpath_opt;

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
  node.jobq.put("evict");
  send_raw(res, wire_fin());
}

// DELETE /{bucket}/{key}: drop entry from local cache.
static void handle_delete(node_t &node, const httplib::Request &req,
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
  const relpath_t &relpath = *relpath_opt;
  node.cache.drop(relpath);
  send_raw(res, wire_fin());
}

// ── Server lifecycle
// ──────────────────────────────────────────────────────────

// Register all routes and start listening on nodeid's host:port (blocks).
void http_server_t::start(const std::string &nodeid) {
  auto [addr, fid] = membership_t::parse_peer(nodeid);
  size_t colon = addr.rfind(':');
  std::string host = addr.substr(0, colon);
  int port = std::stoi(addr.substr(colon + 1));

  start_time_ = time(nullptr);
  svr_ = std::make_unique<httplib::Server>();

  svr_->Get("/_health",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_health(node_, req, res, start_time_);
            });
  svr_->Get("/_nodes",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_nodes(node_, req, res);
            });
  svr_->Post("/_claim_keyspace",
             [this](const httplib::Request &req, httplib::Response &res) {
               handle_claim_keyspace(node_, req, res);
             });
  svr_->Post("/_hint_pull",
             [this](const httplib::Request &req, httplib::Response &res) {
               handle_hint_pull(node_, req, res);
             });
  svr_->Get(R"(/[^/]+/.+)",
            [this](const httplib::Request &req, httplib::Response &res) {
              handle_get(node_, req, res);
            });
  svr_->Put(R"(/[^/]+/.+)",
            [this](const httplib::Request &req, httplib::Response &res,
                   const httplib::ContentReader &content_reader) {
              handle_put(node_, req, res, content_reader);
            });
  svr_->Delete(R"(/[^/]+/.+)",
               [this](const httplib::Request &req, httplib::Response &res) {
                 handle_delete(node_, req, res);
               });

  svr_->listen(host, port);
}

// Signal httplib server to stop accepting connections.
void http_server_t::stop() {
  if (svr_)
    svr_->stop();
}
