#include "node.h"
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

static bool mkdir_p(const std::string &path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

static bool wipe_dir(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  if (ec)
    return false;
  return mkdir_p(path);
}

// Wipe cache/ once if it lacks the layout marker cache/.v2, i.e. it holds
// files of the old layout (cache/bucket/key, with no "^^XXXXXXXX" suffix).
// Old files cannot be told apart by name, so they all go; the cache is cold
// once. The startup walk skips dot-files, so the marker never enters the LRU.
static bool upgrade_cache_layout(const std::string &cachedir) {
  std::string marker = cachedir + "/.v2";
  if (std::filesystem::exists(marker))
    return true;
  if (!wipe_dir(cachedir))
    return false;
  FILE *f = fopen(marker.c_str(), "w");
  if (!f)
    return false;
  fclose(f);
  return true;
}

bool node_t::start() {
  // create required directories
  if (!mkdir_p(abspath("log"))) {
    fprintf(stderr, "failed to create %s/log\n", homedir.c_str());
    return false;
  }
  if (!mkdir_p(abspath("cache"))) {
    fprintf(stderr, "failed to create %s/cache\n", homedir.c_str());
    return false;
  }
  // drop old-layout files before the scan and before http serves anything
  if (!upgrade_cache_layout(abspath("cache"))) {
    fprintf(stderr, "failed to upgrade %s/cache layout\n", homedir.c_str());
    return false;
  }
  if (!wipe_dir(abspath("tmp"))) {
    fprintf(stderr, "failed to create %s/tmp\n", homedir.c_str());
    return false;
  }

  // start rebalancer service
  rebalancer.start();

  // start membership service
  if (!membership.start(nodeid, seeds)) {
    fprintf(stderr, "failed to start membership on %s\n",
            nodeid_lookup(nodeid));
    rebalancer.stop();
    return false;
  }

  // scan existing cache files in background
  std::thread([this]() {
    constexpr size_t BATCH = 1024;
    std::vector<std::pair<std::string, time_t>> batch;
    batch.reserve(BATCH);
    cache.scan_cache([&](const std::string &relpath, time_t mtime) {
      batch.push_back({relpath, mtime});
      if (batch.size() >= BATCH) {
        cache.batch_put(batch);
        batch.clear();
      }
    });
    if (!batch.empty())
      cache.batch_put(batch);
  }).detach();

  // start http service in background thread
  thread_ = std::thread([this]() { http.start(nodeid); });

  return true;
}

void node_t::stop() {
  http.stop();
  feed.stop();
  membership.leave();
  rebalancer.stop();
}

void node_t::join() {
  if (thread_.joinable())
    thread_.join();
}
