#include "node.h"
#include "pidlock.h"
#include "s3copy.h"
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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define DISTCACHE_VERSION "0.1.0"

// Launch s3copy if it is not already responding on /ping.
// s3copy.py is expected to live in the same directory as this binary.
// Uses s3copy's -d flag so it double-forks into an independent daemon.
// Returns true if s3copy was launched successfully, false otherwise.
static bool maybe_launch_s3copy(const std::string &logdir) {
  if (s3copy_ping()) {
    spdlog::info("s3copy already running");
    return false;
  }

  // Locate s3copy.py next to this executable.
  char exe[4096] = {};
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    spdlog::warn("s3copy: cannot resolve binary path; skipping auto-launch");
    return false;
  }
  exe[n] = '\0';
  char *slash = strrchr(exe, '/');
  if (!slash) {
    spdlog::warn("s3copy: unexpected exe path {}; skipping auto-launch", exe);
    return false;
  }
  *slash = '\0';
  std::string script = std::string(exe) + "/s3copy.py";
  if (access(script.c_str(), F_OK) != 0) {
    spdlog::warn("s3copy not running; {} not found; skipping auto-launch",
                 script);
    return false;
  }

  // Fork and exec; s3copy's -d flag double-forks into a daemon.
  // The first-generation child exits quickly and we reap it here.
  pid_t pid = fork();
  if (pid < 0) {
    spdlog::warn("s3copy: fork failed: {}", strerror(errno));
    return false;
  }
  if (pid == 0) {
    execlp("python3", "python3", script.c_str(), "-L", logdir.c_str(), "-d",
           nullptr);
    _exit(1);
  }
  waitpid(pid, nullptr, 0);
  spdlog::info("s3copy launched from {}", script);
  return true;
}

static sem_t g_stop_sem;
// Wake main thread; sem_post is async-signal-safe.
static void on_signal(int) { sem_post(&g_stop_sem); }

// Print usage message to stderr.
static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s -D <homedir> -N <host:port[/first_pos]> [-p <seed>]...\n"
          "\n"
          "distcache is a node in a distributed cache cluster. Each node\n"
          "stores its cached objects under <homedir> and serves them over\n"
          "HTTP, redirecting requests to the node responsible for a key.\n"
          "\n"
          "  -D <homedir>          directory to store this node's cache,\n"
          "                        logs, and lock file\n"
          "  -N <host:port[/first_pos]>\n"
          "                        address this node listens on; an\n"
          "                        optional /first_pos sets its first ring\n"
          "                        position instead of deriving one\n"
          "  -p <seed>             address of an existing cluster member\n"
          "                        to join through; may be repeated\n",
          prog);
}

// Parse argc/argv; populate homedir, mynodeid, and seeds.
// Returns false if required flags (-D, -N) are missing or an unknown
// flag is encountered. Returns true with empty homedir on --version.
static bool parse_arg(int argc, char **argv, std::string &homedir,
                      std::string &mynodeid, std::vector<std::string> &seeds) {
  homedir = "";
  mynodeid = "";
  seeds.clear();
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      homedir = argv[++i];
    } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
      mynodeid = argv[++i];
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
  if (homedir.empty() || mynodeid.empty()) {
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  std::string homedir;
  std::string mynodeid;
  std::vector<std::string> seeds;
  std::error_code ec;

  if (!parse_arg(argc, argv, homedir, mynodeid, seeds)) {
    usage(argv[0]);
    return 1;
  }

  nodeid_idx_t nid = nodeid_find(mynodeid.c_str());
  if (!nodeid_valid(nid)) {
    fprintf(stderr, "Error: nodeid pool full, cannot register %s\n",
            mynodeid.c_str());
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

  // Initialize s3copy module from $S3COPY_URL.
  s3copy_init();

  // Start s3copy sidecar if not already running.
  maybe_launch_s3copy(node.abspath("log"));

  // Wait for s3copy: up to 10 attempts, 200 ms apart (~2 s total).
  {
    bool ready = false;
    for (int i = 0; i < 10; i++) {
      if (s3copy_ping()) {
        ready = true;
        break;
      }
      usleep(200000);
    }
    if (!ready) {
      spdlog::error("s3copy not responding after 10 attempts; aborting");
      return 1;
    }
  }

  if (!node.start()) {
    return 1;
  }

  spdlog::info("distcache {} starting on {} homedir={}", DISTCACHE_VERSION,
               mynodeid, homedir);

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
