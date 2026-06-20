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
  std::string homedir;            // absolute root; all file ops use abspath()
  std::string nodeid;             // "host:port" or "host:port/first_id"
  std::vector<std::string> seeds; // SWIM bootstrap peers
  keylock_t keylock;              // serialises concurrent S3 fetches per key
  cache_t cache;                  // file cache; owns lru_t internally
  Stats stats;                    // atomic hit/miss/redirect/error counters
  std::atomic<std::shared_ptr<ring_t>> ring{
      std::make_shared<ring_t>()}; // lock-free ring; swapped on membership
                                   // change
  feed_t feed; // SWIM events + app telemetry; drives membership_t consumer
  feed_t jobq; // async job queue (hint_pull, do_pull, claim_scan, evict);
               // drives rebalancer_t consumer
  membership_t
      membership; // SWIM lifecycle, ring rebuild, pre-convergence injection
  rebalancer_t rebalancer; // pull_one, claim_scan, hint_pull jobs
  http_server_t http;      // cpp-httplib thread-per-request server

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
