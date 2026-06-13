#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <swim.h>
#include <thread>
#include <utility>
#include <vector>

// Wraps swim_feed_t with a background consumer thread.
// Records arrive as vector<string>; the first string is the event type.
using FeedCallback = std::function<void(std::vector<std::string>)>;

class Feed {
public:
  Feed();
  ~Feed();

  // Start the consumer thread. callback is called for each record.
  void start(FeedCallback callback);

  // Signal shutdown and join consumer thread.
  void stop();

  // Emit a record with 1..SWIM_FEED_MAX_ELEMENTS const char* args.
  template <typename... Args> bool put(Args &&...args) {
    return swim_feed_put(feed_, static_cast<int>(sizeof...(args)),
                         std::forward<Args>(args)...) == 0;
  }

  swim_feed_t *raw() { return feed_; }

private:
  swim_feed_t *feed_ = nullptr;
  std::thread thread_;
  bool running_ = false;
};

// Global feed shared by SWIM and app telemetry.
extern Feed g_feed;
