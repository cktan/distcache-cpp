#include "feed.h"
#include <cstdio>
#include <ctime>

// Create underlying swim feed handle.
feed_t::feed_t() { feed_ = swim_feed_create(); }

// Stop background thread (if running) and destroy the swim feed handle.
feed_t::~feed_t() {
  if (running_)
    stop();
  if (feed_)
    swim_feed_destroy(feed_);
}

// Spawn background thread that polls the swim feed and invokes callback
// for each batch of records until stop() is called.
void feed_t::start(feed_callback_t callback) {
  running_ = true;
  thread_ = std::thread([this, cb = std::move(callback)]() {
    char buf[SWIM_FEED_MAX_RECORD_SIZE];
    char *ptrs[SWIM_FEED_MAX_ELEMENTS];

    while (running_) {
      int rc = swim_feed_wait(feed_, 500);
      if (rc < 0)
        break;

      // Drain all available records.
      while (running_) {
        int n = swim_feed_get(feed_, sizeof(buf), buf, SWIM_FEED_MAX_ELEMENTS,
                              ptrs);
        if (n == 0)
          break;
        if (n < 0)
          continue;

        std::vector<std::string> rec;
        rec.reserve(n);
        for (int i = 0; i < n; i++)
          rec.emplace_back(ptrs[i]);
        cb(std::move(rec));
      }
    }
  });
}

// Signal background thread to exit, wake it from swim_feed_wait, and join.
void feed_t::stop() {
  running_ = false;
  swim_feed_wakeall(feed_);
  if (thread_.joinable())
    thread_.join();
}
