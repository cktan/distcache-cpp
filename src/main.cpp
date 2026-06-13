#include "cache.h"
#include "config.h"
#include "feed.h"
#include "http.h"
#include "lru.h"
#include "membership.h"
#include "pidlock.h"
#include "rebalance.h"
#include "relpath.h"
#include "ring.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <vector>

#define DISTCACHE_VERSION "0.1.0"

Config g_config;

// ── Signal handler
// ────────────────────────────────────────────────────────────

static void on_signal(int) { g_http.stop(); }

// ── Directory helpers
// ─────────────────────────────────────────────────────────

static bool mkdir_p(const std::string &path) {
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
    return true;
  return false;
}

static bool wipe_dir(const std::string &path) {
  // Remove directory and recreate it.
  std::string cmd = "rm -rf " + path;
  if (system(cmd.c_str()) != 0)
    return false;
  return mkdir_p(path);
}

// ── Flag parsing
// ──────────────────────────────────────────────────────────────

static void usage(const char *prog) {
  fprintf(
      stderr,
      "Usage: %s -D <homedir> -N <host:port> [-I <firstid>] [-p <seed>]...\n",
      prog);
}

static bool parse_addr(const std::string &s, std::string &host,
                       uint16_t &port) {
  size_t colon = s.rfind(':');
  if (colon == std::string::npos)
    return false;
  host = s.substr(0, colon);
  std::string port_s = s.substr(colon + 1);
  char *end;
  long p = strtol(port_s.c_str(), &end, 10);
  if (*end || p <= 0 || p > 65535)
    return false;
  port = static_cast<uint16_t>(p);
  return !host.empty();
}

// ── Main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  std::string homedir;
  std::string addr_str;
  uint32_t first_id = 0;
  std::vector<std::string> seeds;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      homedir = argv[++i];
    } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
      addr_str = argv[++i];
    } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
      char *end;
      unsigned long v = strtoul(argv[++i], &end, 10);
      if (*end) {
        fprintf(stderr, "bad -I value\n");
        return 1;
      }
      first_id = static_cast<uint32_t>(v);
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      seeds.push_back(argv[++i]);
    } else if (strcmp(argv[i], "--version") == 0) {
      printf("distcache %s\n", DISTCACHE_VERSION);
      return 0;
    } else {
      fprintf(stderr, "unknown flag: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if (homedir.empty() || addr_str.empty()) {
    usage(argv[0]);
    return 1;
  }

  std::string host;
  uint16_t port;
  if (!parse_addr(addr_str, host, port)) {
    fprintf(stderr, "bad -N value: %s\n", addr_str.c_str());
    return 1;
  }

  // ── Populate global config ────────────────────────────────────────────────
  g_config.host = host;
  g_config.port = port;
  g_config.addr = addr_str;
  g_config.homedir = homedir;
  g_config.first_id = first_id;

  // ── Create directory structure ────────────────────────────────────────────
  if (!mkdir_p(homedir) || !mkdir_p(homedir + "/log") ||
      !mkdir_p(homedir + "/cache") || !wipe_dir(homedir + "/tmp")) {
    fprintf(stderr, "failed to create homedir structure: %s\n",
            strerror(errno));
    return 1;
  }

  // ── PID lock ──────────────────────────────────────────────────────────────
  std::string lockpath = homedir + "/dist_cache.lock";
  if (!pidlock_acquire(lockpath)) {
    fprintf(stderr, "another instance holds %s\n", lockpath.c_str());
    return 1;
  }

  // ── Signal handlers ───────────────────────────────────────────────────────
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  // ── Rebalancer (g_jobq worker) ────────────────────────────────────────────
  g_rebalancer.start();

  // ── SWIM membership + feed ────────────────────────────────────────────────
  if (!g_membership.start(host, port, first_id, seeds)) {
    g_rebalancer.stop();
    pidlock_release(lockpath);
    return 1;
  }

  // ── Background cache scan ─────────────────────────────────────────────────
  std::thread scan_thread([] {
    constexpr size_t BATCH = 1024;
    std::vector<std::pair<std::string, time_t>> batch;
    batch.reserve(BATCH);

    scan_cache([&](const std::string &relpath, time_t mtime) {
      batch.push_back({relpath, mtime});
      if (batch.size() >= BATCH) {
        g_lru.scan_insert(batch, Ring::compute_relpath_id);
        batch.clear();
      }
    });
    if (!batch.empty())
      g_lru.scan_insert(batch, Ring::compute_relpath_id);

    fprintf(stderr, "startup scan complete: %zu entries\n", g_lru.size());
  });
  scan_thread.detach();

  // ── HTTP server (blocks until SIGTERM) ────────────────────────────────────
  fprintf(stderr, "distcache %s starting on %s:%d homedir=%s\n",
          DISTCACHE_VERSION, host.c_str(), port, homedir.c_str());

  g_http.start(host, port);

  // ── Shutdown sequence ─────────────────────────────────────────────────────
  g_membership.leave();
  g_feed.stop();
  g_rebalancer.stop();
  pidlock_release(lockpath);

  fprintf(stderr, "distcache stopped\n");
  return 0;
}
