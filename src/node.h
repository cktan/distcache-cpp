#pragma once
#include "cache.h"
#include "config.h"
#include "feed.h"
#include "http.h"
#include "keylock.h"
#include "lru.h"
#include "membership.h"
#include "rebalance.h"
#include "ring.h"
#include "stats.h"
#include <atomic>
#include <memory>

struct Node {
  Config config;
  cache_t cache;
  Stats stats;
  std::atomic<std::shared_ptr<Ring>> ring{std::make_shared<Ring>()};
  keylock_t keylock;
  feed_t feed;
  feed_t jobq;
  lru_t lru;
  Membership membership;
  Rebalancer rebalancer;
  HttpServer http;

  Node()
      : lru(config.homedir, keylock, Ring::compute_relpath_id),
        membership(*this), rebalancer(*this), http(*this) {}
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
};
