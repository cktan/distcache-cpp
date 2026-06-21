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
