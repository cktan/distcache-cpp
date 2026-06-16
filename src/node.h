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

struct node_t {
  std::string nodeid; // "host:port" or "host:port/first_id"
  keylock_t keylock;
  cache_t cache;
  Stats stats;
  std::atomic<std::shared_ptr<ring_t>> ring{std::make_shared<ring_t>()};
  feed_t feed;
  feed_t jobq;
  membership_t membership;
  rebalancer_t rebalancer;
  http_server_t http;

  node_t(const std::string &nodeid)
      : nodeid(nodeid), cache(keylock), membership(*this), rebalancer(*this),
        http(*this) {}
  node_t(const node_t &) = delete;
  node_t &operator=(const node_t &) = delete;
};
