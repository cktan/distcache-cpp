#include "feed.h"
#include <cstdio>
#include <ctime>

feed_t::feed_t() { feed_ = swim_feed_create(); }

feed_t::~feed_t() {
  if (running_)
    stop();
  if (feed_)
    swim_feed_destroy(feed_);
}

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

void feed_t::stop() {
  running_ = false;
  swim_feed_wakeall(feed_);
  if (thread_.joinable())
    thread_.join();
}
