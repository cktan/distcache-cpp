#pragma once
#include "cache.h"
#include "feed.h"
#include "http.h"
#include "keylock.h"
#include "membership.h"
#include "rebalance.h"
#include "ring.h"
#include "stats.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct node_t {
  std::string homedir;
  std::string nodeid; // "host:port" or "host:port/first_id"
  std::vector<std::string> seeds;
  keylock_t keylock;
  cache_t cache;
  Stats stats;
  std::atomic<std::shared_ptr<ring_t>> ring{std::make_shared<ring_t>()};
  feed_t feed;
  feed_t jobq;
  membership_t membership;
  rebalancer_t rebalancer;
  http_server_t http;

  node_t(const std::string &homedir, const std::string &nodeid)
      : homedir(homedir), nodeid(nodeid), cache(keylock, homedir),
        membership(*this), rebalancer(*this), http(*this) {}
  node_t(const node_t &) = delete;
  node_t &operator=(const node_t &) = delete;

  std::string abspath(const std::string &relpath) const {
    return homedir + "/" + relpath;
  }

  // Create dirs, start all services, run http in a background thread.
  // Returns false if a service fails to start.
  bool start();

  // Signal all services to stop.
  void stop();

  // Block until the http thread exits. Call after stop().
  void join();

private:
  std::thread thread_;
};
