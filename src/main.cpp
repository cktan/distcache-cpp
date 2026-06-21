#include "node.h"
#include "pidlock.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <semaphore.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <vector>

#define DISTCACHE_VERSION "0.1.0"

static sem_t g_stop_sem;
// Wake main thread; sem_post is async-signal-safe.
static void on_signal(int) { sem_post(&g_stop_sem); }

// Print a usage message
static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s -D <homedir> -N <host:port[/first_id]> [-p <seed>]...\n"
          "\n"
          "distcache is a node in a distributed cache cluster. Each node\n"
          "stores its cached objects under <homedir> and serves them over\n"
          "HTTP, redirecting requests to the node responsible for a key.\n"
          "\n"
          "  -D <homedir>          directory to store this node's cache,\n"
          "                        logs, and lock file\n"
          "  -N <host:port[/first_id]>\n"
          "                        address this node listens on; an\n"
          "                        optional /first_id sets its first ring\n"
          "                        id instead of deriving one\n"
          "  -p <seed>             address of an existing cluster member\n"
          "                        to join through; may be repeated\n",
          prog);
}

// Parse argc/argv to extract parameters
static bool parse_arg(int argc, char **argv, std::string &homedir,
                      std::string &addr_str, std::vector<std::string> &seeds) {
  homedir = "";
  addr_str = "";
  seeds.clear();
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

  nodeid_idx_t nid = nodeid_register(addr_str.c_str());
  if (!nodeid_valid(nid)) {
    fprintf(stderr, "Error: nodeid pool full, cannot register %s\n",
            addr_str.c_str());
    return 1;
  }
  node_t node(homedir, nid);
  node.seeds = seeds;

  // Create log dir before setting up the logger (sink needs the dir to exist).
  std::filesystem::create_directories(node.abspath("log"), ec);
  if (ec) {
    fprintf(stderr, "Error: failed to create log dir: %s\n",
            ec.message().c_str());
    return 1;
  }

  // Both loggers share file_sink so every message lands in the log
  // file; startup_logger additionally echoes to the console until
  // the server starts serving, then we drop to file_logger only.
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      node.abspath("log/distcache.log"));
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto startup_logger = std::make_shared<spdlog::logger>(
      "distcache", spdlog::sinks_init_list{file_sink, console_sink});
  auto file_logger = std::make_shared<spdlog::logger>(
      "distcache", spdlog::sinks_init_list{file_sink});
  spdlog::set_default_logger(startup_logger);

  // Set lock file so no one can start another distcache on
  // this homedir.
  std::string lockpath = node.abspath("dist_cache.lock");
  if (!pidlock_acquire(lockpath)) {
    spdlog::error("another instance holds {}", lockpath);
    return 1;
  }

  // Initialize stop semaphore and set signal handlers
  sem_init(&g_stop_sem, 0, 0);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  if (!node.start()) {
    return 1;
  }

  spdlog::info("distcache {} starting on {} homedir={}", DISTCACHE_VERSION,
               addr_str, homedir);

  // Starting here, write logs to file only
  spdlog::set_default_logger(file_logger);

  // Block until signaled, then stop all services.
  while (sem_wait(&g_stop_sem) == -1 && errno == EINTR)
    ;
  node.stop();
  node.join();
  sem_destroy(&g_stop_sem);

  spdlog::info("distcache stopped");
  return 0;
}
