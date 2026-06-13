#include "feed.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>

Feed g_feed;

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

bool Feed::put(int n, ...) {
  va_list ap;
  va_start(ap, n);

  // Collect args; swim_feed_put is variadic so we rebuild the call.
  // Max 10 strings per SWIM_FEED_MAX_ELEMENTS.
  const char *args[SWIM_FEED_MAX_ELEMENTS];
  if (n < 1 || n > SWIM_FEED_MAX_ELEMENTS) {
    va_end(ap);
    return false;
  }
  for (int i = 0; i < n; i++)
    args[i] = va_arg(ap, const char *);
  va_end(ap);

  // swim_feed_put is variadic; build the call for up to 10 args.
  switch (n) {
  case 1:
    return swim_feed_put(feed_, 1, args[0]) == 0;
  case 2:
    return swim_feed_put(feed_, 2, args[0], args[1]) == 0;
  case 3:
    return swim_feed_put(feed_, 3, args[0], args[1], args[2]) == 0;
  case 4:
    return swim_feed_put(feed_, 4, args[0], args[1], args[2], args[3]) == 0;
  case 5:
    return swim_feed_put(feed_, 5, args[0], args[1], args[2], args[3],
                         args[4]) == 0;
  default:
    return false;
  }
}
