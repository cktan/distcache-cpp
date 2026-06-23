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
#include <fcntl.h>
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
    std::string err = wire_err("ERR_IO", strerror(errno)) + WIRE_FIN;
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
  fd_t fd;

  // Obtain bucket & key, form relpath, and compute relpath_id.
  // We need the relpath_id to interrogate ring for the responsible node.
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("ERR_INVALID_PATH", "invalid path") + WIRE_FIN);
    return;
  }

  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res,
             wire_err("ERR_INVALID_PATH", "invalid bucket or key") + WIRE_FIN);
    return;
  }
  const relpath_t &relpath = *relpath_opt;
  uint32_t rp_id = ring_compute_id(relpath.str().c_str());

  // Check for flags
  bool local_only = req.has_param("local");
  bool no_redirect = req.has_param("no_redirect");
  bool delete_after = req.has_param("delete");

  if (!local_only && !no_redirect) {
    // The normal case! We need to redirect if this node is not dest.

    auto ring = node.ring.load();
    nodeid_idx_t dest_idx = ring_successor(ring.get(), rp_id);
    if (nodeid_valid(dest_idx) && !nodeid_eq(dest_idx, node.nodeid)) {
      // Key belongs to another ring node; look up string only for network ops.
      char host[254];
      int port = 0;
      nodeid_split(dest_idx, host, &port, nullptr);
      std::string dest_addr = std::string(host) + ":" + std::to_string(port);
      node.membership.hint_alive(dest_addr);
      const char *dest_c = nodeid_lookup(dest_idx);
      node.feed.put("cache.redirect", relpath.str().c_str(), "dest",
                    dest_c ? dest_c : "");

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
      send_raw(res, wire_redirect(dest_c ? dest_c : "") + WIRE_FIN);
      return;
    }
  }

  // This node is dest (or is forced to be dest).

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
    send_raw(res, wire_err("ERR_NOT_CACHED", "key not cached") + WIRE_FIN);
    return;
  }

  // Grab keylock before attempting to download from S3
  keylock_guard_t kl_guard(node.keylock, relpath.str());

  // Another thread may have fetched while we waited. Open under lock so the
  // fd keeps the inode alive, then release the lock before reading content.
  if (node.cache.read(relpath, fd).ok()) {
    kl_guard.release();
    node.stats.served++;
    serve_fd(fd, res);
    return;
  }

  // Set up to download from S3 into a temp file
  relpath_t tmp_rel = node.cache.tmp_relpath();
  std::string tmp_abs = node.cache.abspath(tmp_rel);
  rc_t rc = s3copy_fetch(bucket, key, tmp_abs);
  if (!rc.ok()) {
    remove(tmp_abs.c_str());
    kl_guard.release();
    node.stats.errors++;
    node.feed.put("s3copy.err", relpath.str().c_str(), rc.errstr());
    send_raw(res, wire_err("ERR_S3_COPY", "s3copy failed") + WIRE_FIN);
    return;
  }

  // Rename the temp file into cache
  rc = node.cache.move(tmp_rel, relpath);
  if (!rc.ok()) {
    remove(tmp_abs.c_str());
    kl_guard.release();
    node.stats.errors++;
    send_raw(res, wire_err("ERR_IO", "move failed") + WIRE_FIN);
    return;
  }
  kl_guard.release();

  // s3 copy succeeded.
  node.feed.put("s3copy.ok", relpath.str().c_str());
  node.jobq.put("maybe_evict");

  // Open file and serve
  if (!node.cache.read(relpath, fd).ok()) {
    node.stats.errors++;
    send_raw(res, wire_err("ERR_IO", "read after fetch failed") + WIRE_FIN);
    return;
  }
  node.stats.served++;
  serve_fd(fd, res);
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
  size_t nodes = ring_node_count(ring.get());

  char used_gib_buf[32];
  snprintf(used_gib_buf, sizeof(used_gib_buf), "%.2f",
           node.cache.disk_used_gib());

  res.status = 200;
  res.set_header("Content-Type", "application/json");
  const char *self_id = nodeid_lookup(node.nodeid);
  res.body = std::format(
      R"({{"addr":"{}","upsince":"{}","upfor":"{}","nodes":{},"lru_size":{},"disk_pct":{},"used_gib":{},"df_error":{},"hits":{},"misses":{},"served":{},"redirects":{},"errors":{},"hint_pull":{},"peers":{}}})",
      self_id ? self_id : "", upsince, upfor, nodes, node.cache.size(),
      node.cache.disk_usepct(), used_gib_buf,
      node.cache.disk_error() ? "true" : "false", node.stats.hits.load(),
      node.stats.misses.load(), node.stats.served.load(),
      node.stats.redirects.load(), node.stats.errors.load(),
      node.stats.hint_pull.load(), peers_json);
}

// GET /_nodes: return all known ring members as newline-separated nodeids.
static void handle_nodes(node_t &node, const httplib::Request &,
                         httplib::Response &res) {
  auto peers = node.membership.raw_peers();
  std::string body;
  for (const auto &p : peers) {
    body += p;
    body += '\n';
  }
  res.status = 200;
  res.set_header("Content-Type", "text/plain");
  res.body = std::move(body);
}

// POST /_claim_keyspace: enqueue claim_scan for a joining node's keyspace.
static void handle_claim_keyspace(node_t &node, const httplib::Request &req,
                                  httplib::Response &res) {
  std::string nodeid = json_str(req.body, "nodeid");

  if (nodeid.empty()) {
    send_raw(res, wire_err("ERR_BAD_REQUEST", "missing nodeid") + WIRE_FIN);
    return;
  }

  node.jobq.put("claim_scan", nodeid.c_str());
  send_raw(res, WIRE_FIN);
}

// POST /_hint_pull: enqueue do_pull so this node fetches relpath from src.
static void handle_hint_pull(node_t &node, const httplib::Request &req,
                             httplib::Response &res) {
  std::string src = json_str(req.body, "src");
  std::string relpath = json_str(req.body, "relpath");
  std::string mtime = json_str(req.body, "mtime");

  if (src.empty() || relpath.empty()) {
    send_raw(res,
             wire_err("ERR_BAD_REQUEST", "missing src or relpath") + WIRE_FIN);
    return;
  }

  node.stats.hint_pull++;
  node.jobq.put("do_pull", src.c_str(), relpath.c_str(), mtime.c_str());
  send_raw(res, WIRE_FIN);
}

// PUT /{bucket}/{key}: stream request body into cache and trigger eviction.
// NOTE: this is used by tests only.
static void handle_put(node_t &node, const httplib::Request &req,
                       httplib::Response &res,
                       const httplib::ContentReader &content_reader) {
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("ERR_INVALID_PATH", "invalid path") + WIRE_FIN);
    return;
  }
  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res,
             wire_err("ERR_INVALID_PATH", "invalid bucket or key") + WIRE_FIN);
    return;
  }
  const relpath_t &relpath = *relpath_opt;

  relpath_t tmp_rel = node.cache.tmp_relpath();
  int raw_fd = ::open(node.cache.abspath(tmp_rel).c_str(),
                      O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (raw_fd < 0) {
    send_raw(res, wire_err("ERR_IO", "write failed") + WIRE_FIN);
    return;
  }
  fd_t fd(raw_fd);

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
    node.cache.drop(tmp_rel);
    send_raw(res, wire_err("ERR_IO", "write failed") + WIRE_FIN);
    return;
  }
  if (!node.cache.move(tmp_rel, relpath).ok()) {
    send_raw(res, wire_err("ERR_IO", "write failed") + WIRE_FIN);
    return;
  }
  node.jobq.put("maybe_evict");
  send_raw(res, WIRE_FIN);
}

// DELETE /{bucket}/{key}: drop entry from local cache.
static void handle_delete(node_t &node, const httplib::Request &req,
                          httplib::Response &res) {
  std::string bucket, key;
  if (!split_path(req.path, bucket, key)) {
    send_raw(res, wire_err("ERR_INVALID_PATH", "invalid path") + WIRE_FIN);
    return;
  }
  auto relpath_opt = compute_relpath(bucket, key);
  if (!relpath_opt) {
    send_raw(res,
             wire_err("ERR_INVALID_PATH", "invalid bucket or key") + WIRE_FIN);
    return;
  }
  const relpath_t &relpath = *relpath_opt;
  node.cache.drop(relpath);
  send_raw(res, WIRE_FIN);
}

// ── Server lifecycle
// ──────────────────────────────────────────────────────────

// Register all routes and start listening on nodeid's host:port (blocks).
void http_server_t::start(nodeid_idx_t nodeid) {
  const char *nodeid_str = nodeid_lookup(nodeid);
  char host[254];
  int port = 0;
  if (nodeid_split(nodeid, host, &port, nullptr) != 0) {
    fprintf(stderr, "http_server.start: bad nodeid: %s\n",
            nodeid_str ? nodeid_str : "(null)");
    return;
  }

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
