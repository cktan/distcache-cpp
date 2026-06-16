#include "node.h"
#include "pidlock.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

#define DISTCACHE_VERSION "0.1.0"

// Signal handlers can't capture state, so the server to stop is reached
// through a global instead of being passed in.
static HttpServer *g_http_signal = nullptr;
static void on_signal(int) {
  if (g_http_signal)
    g_http_signal->stop();
}

// ec is an out-param rather than relying on errno: std::filesystem
// operations report failures via std::error_code, not errno.
static bool mkdir_p(const std::string &path, std::error_code &ec) {
  std::filesystem::create_directories(path, ec);
  return !ec;
}

static bool wipe_dir(const std::string &path, std::error_code &ec) {
  std::filesystem::remove_all(path, ec);
  if (ec)
    return false;
  return mkdir_p(path, ec);
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s -D <homedir> -N <host:port[/first_id]> [-p <seed>]...\n",
          prog);
}

static bool parse_arg(int argc, char **argv, std::string &homedir,
                      std::string &addr_str, std::vector<std::string> &seeds) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      homedir = argv[++i];
    } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
      addr_str = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      seeds.push_back(argv[++i]);
    } else if (strcmp(argv[i], "--version") == 0) {
      printf("distcache %s\n", DISTCACHE_VERSION);
      return true;
    } else {
      fprintf(stderr, "unknown flag: %s\n", argv[i]);
      return false;
    }
  }
  if (homedir.empty() || addr_str.empty()) {
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  std::string homedir;
  std::string addr_str;
  std::vector<std::string> seeds;
  std::error_code ec;

  if (!parse_arg(argc, argv, homedir, addr_str, seeds)) {
    usage(argv[0]);
    return 1;
  }

  // chdir into homedir
  std::filesystem::current_path(homedir, ec);
  if (ec) {
    fprintf(stderr, "Error: failed to chdir into homedir %s: %s\n",
            homedir.c_str(), ec.message().c_str());
    return 1;
  }
  // homedir is now the cwd, so every path built from it downstream
  // (Node, mkdir_p, lockpath, log files) only needs to be relative.
  homedir = "./";

  Node node(homedir, addr_str);

  if (!mkdir_p("./log", ec)) {
    fprintf(stderr, "Error: failed to create ./log: %s\n",
            ec.message().c_str());
    return 1;
  }
  if (!mkdir_p("./cache", ec)) {
    fprintf(stderr, "Error: failed to create ./cache: %s\n",
            ec.message().c_str());
    return 1;
  }
  if (!wipe_dir("./tmp", ec)) {
    fprintf(stderr, "Error: failed to create ./tmp: %s\n",
            ec.message().c_str());
    return 1;
  }

  // Both loggers share file_sink so every message lands in the log
  // file; startup_logger additionally echoes to the console until
  // the server starts serving, then we drop to file_logger only.
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      "./log/distcache.log");
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto startup_logger = std::make_shared<spdlog::logger>(
      "distcache", spdlog::sinks_init_list{file_sink, console_sink});
  auto file_logger = std::make_shared<spdlog::logger>(
      "distcache", spdlog::sinks_init_list{file_sink});
  spdlog::set_default_logger(startup_logger);

  // Set lock file so no one can start another distcache on
  // this homedir.
  std::string lockpath = "./dist_cache.lock";
  if (!pidlock_acquire(lockpath)) {
    spdlog::error("another instance holds {}", lockpath);
    return 1;
  }

  // Set signal handlers
  g_http_signal = &node.http;
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  // Start the rebalancer service
  node.rebalancer.start();

  // Start the membership service
  if (!node.membership.start(addr_str, seeds)) {
    spdlog::error("failed to start membership on {}", addr_str);
    node.rebalancer.stop();
    pidlock_release(lockpath);
    return 1;
  }

  spdlog::info("distcache {} starting on {} homedir={}", DISTCACHE_VERSION,
               addr_str, std::filesystem::current_path().string());

  // Starting here, write logs to file only
  spdlog::set_default_logger(file_logger);

  // Start a thread to scan the files in ./cache
  std::thread scan_thread([&node]() {
    constexpr size_t BATCH = 1024;
    std::vector<std::pair<std::string, time_t>> batch;
    batch.reserve(BATCH);

    node.cache.scan_cache([&](const std::string &relpath, time_t mtime) {
      batch.push_back({relpath, mtime});
      if (batch.size() >= BATCH) {
        node.cache.batch_put(batch);
        batch.clear();
      }
    });
    if (!batch.empty())
      node.cache.batch_put(batch);

    spdlog::info("startup scan complete: {} entries", node.cache.size());
  });
  scan_thread.detach();

  // Start http service. This call will return when http service stops.
  node.http.start(addr_str);

  spdlog::info("distcache stopping ...");
  node.feed.stop();
  node.membership.leave();
  node.rebalancer.stop();
  pidlock_release(lockpath);

  spdlog::info("distcache stopped");
  return 0;
}
