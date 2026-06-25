#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <swim.h>
#include <thread>
#include <utility>
#include <vector>

// Wraps swim_feed_t with a background consumer thread.
// Records arrive as vector<string>; the first string is the event type.
using feed_callback_t = std::function<void(std::vector<std::string>)>;

// feed_t — SWIM feed queue utility
//
// Wraps a swim_feed_t handle and a background thread that drains it.
// Call start() with a callback to begin receiving events; each invocation
// delivers one batch of records as vector<string> where [0] is the event
// type. Call stop() (or let the destructor do it) to shut down cleanly.
// put() publishes a record into the feed for local consumers.
class feed_t {
public:
  feed_t();
  ~feed_t();

  // Start the consumer thread. callback is called for each record.
  void start(feed_callback_t callback);

  // Signal shutdown and join consumer thread.
  void stop();

  // Enqueue a record with 1..SWIM_FEED_MAX_ELEMENTS const char* args.
  template <typename... Args> bool put(Args &&...args) {
    return swim_feed_put(m_feed_, static_cast<int>(sizeof...(args)),
                         std::forward<Args>(args)...) == 0;
  }

  swim_feed_t *raw() { return m_feed_; }

private:
  swim_feed_t *m_feed_ = nullptr;
  std::thread m_thread_;
  std::atomic<bool> m_running_ = false;
};
