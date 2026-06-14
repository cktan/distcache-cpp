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
  KeyLock keylock;
  Feed feed;
  Feed jobq;
  LRU lru;
  Membership membership;
  Rebalancer rebalancer;
  HttpServer http;

  Node() : lru(*this), membership(*this), rebalancer(*this), http(*this) {}
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
};
