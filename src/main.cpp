#include "node.h"
#include "pidlock.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <thread>
#include <vector>

#define DISTCACHE_VERSION "0.1.0"

static HttpServer *g_http_signal = nullptr;
static void on_signal(int) {
  if (g_http_signal)
    g_http_signal->stop();
}

static bool mkdir_p(const std::string &path) {
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
    return true;
  return false;
}

static bool wipe_dir(const std::string &path) {
  std::filesystem::remove_all(path);
  return mkdir_p(path);
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s -D <homedir> -N <host:port[/first_id]> [-p <seed>]...\n",
          prog);
}

int main(int argc, char **argv) {
  std::string homedir;
  std::string addr_str;
  std::vector<std::string> seeds;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      homedir = argv[++i];
    } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
      addr_str = argv[++i];
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

  Node node(homedir, addr_str);

  if (!mkdir_p(homedir) || !mkdir_p(homedir + "/log") ||
      !mkdir_p(homedir + "/cache") || !wipe_dir(homedir + "/tmp")) {
    fprintf(stderr, "failed to create homedir structure: %s\n",
            strerror(errno));
    return 1;
  }

  std::string lockpath = homedir + "/dist_cache.lock";
  if (!pidlock_acquire(lockpath)) {
    fprintf(stderr, "another instance holds %s\n", lockpath.c_str());
    return 1;
  }

  g_http_signal = &node.http;
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  node.rebalancer.start();

  if (!node.membership.start(addr_str, seeds)) {
    node.rebalancer.stop();
    pidlock_release(lockpath);
    return 1;
  }

  std::thread scan_thread([&node]() {
    constexpr size_t BATCH = 1024;
    std::vector<std::pair<std::string, time_t>> batch;
    batch.reserve(BATCH);

    node.cache.scan_cache([&](const std::string &relpath, time_t mtime) {
      batch.push_back({relpath, mtime});
      if (batch.size() >= BATCH) {
        node.cache.batch_put(batch);
        batch.clear();
      }
    });
    if (!batch.empty())
      node.cache.batch_put(batch);

    fprintf(stderr, "startup scan complete: %zu entries\n", node.cache.size());
  });
  scan_thread.detach();

  fprintf(stderr, "distcache %s starting on %s homedir=%s\n", DISTCACHE_VERSION,
          addr_str.c_str(), homedir.c_str());

  node.http.start(addr_str);

  node.feed.stop();
  node.membership.leave();
  node.rebalancer.stop();
  pidlock_release(lockpath);

  fprintf(stderr, "distcache stopped\n");
  return 0;
}
