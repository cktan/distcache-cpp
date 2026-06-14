#include "feed.h"
#include <cstdio>
#include <ctime>

Feed::Feed() { feed_ = swim_feed_create(); }

Feed::~Feed() {
  if (running_)
    stop();
  if (feed_)
    swim_feed_destroy(feed_);
}

void Feed::start(FeedCallback callback) {
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

void Feed::stop() {
  running_ = false;
  swim_feed_wakeall(feed_);
  if (thread_.joinable())
    thread_.join();
}
