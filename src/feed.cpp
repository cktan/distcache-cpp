#include "feed.h"
#include <cstdio>
#include <ctime>

// Create underlying swim feed handle.
feed_t::feed_t() { m_feed_ = swim_feed_create(); }

// Stop background thread (if running) and destroy the swim feed handle.
feed_t::~feed_t() {
  if (m_running_)
    stop();
  if (m_feed_)
    swim_feed_destroy(m_feed_);
}

// Spawn background thread that polls the swim feed and invokes callback
// for each batch of records until stop() is called.
void feed_t::start(feed_callback_t callback) {
  m_running_ = true;
  m_thread_ = std::thread([this, cb = std::move(callback)]() {
    char buf[SWIM_FEED_MAX_RECORD_SIZE];
    char *ptrs[SWIM_FEED_MAX_ELEMENTS];

    while (m_running_) {
      int rc = swim_feed_wait(m_feed_, 500);
      if (rc < 0)
        break;

      // Drain all available records.
      while (m_running_) {
        int n = swim_feed_get(m_feed_, sizeof(buf), buf, SWIM_FEED_MAX_ELEMENTS,
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
  m_running_ = false;
  swim_feed_wakeall(m_feed_);
  if (m_thread_.joinable())
    m_thread_.join();
}
