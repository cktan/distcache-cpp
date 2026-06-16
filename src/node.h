#pragma once
#include "cache.h"
#include "config.h"
#include "feed.h"
#include "http.h"
#include "keylock.h"
#include "membership.h"
#include "rebalance.h"
#include "ring.h"
#include "stats.h"
#include <atomic>
#include <memory>

struct Node {
  Config config;
  keylock_t keylock;
  cache_t cache;
  Stats stats;
  std::atomic<std::shared_ptr<ring_t>> ring{std::make_shared<ring_t>()};
  feed_t feed;
  feed_t jobq;
  Membership membership;
  Rebalancer rebalancer;
  HttpServer http;

  Node(const std::string &homedir, const std::string &nodeid)
      : cache(keylock), membership(*this), rebalancer(*this),
        http(*this) {
    config.homedir = homedir;
    config.nodeid = nodeid;
  }
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
};
