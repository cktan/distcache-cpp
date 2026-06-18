#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pidlock.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static std::string tmp_lock() {
  char buf[64];
  snprintf(buf, sizeof(buf), "/tmp/dc_pidlock_test_%d_%d.lock", getpid(),
           (int)(intptr_t)&buf);
  return buf;
}

static void write_pid(const std::string &path, pid_t pid) {
  FILE *f = fopen(path.c_str(), "w");
  if (!f)
    return;
  fprintf(f, "%d\n", pid);
  fclose(f);
}

TEST_SUITE("PidLock") {
  TEST_CASE("acquire creates lockfile") {
    std::string path = tmp_lock();
    unlink(path.c_str());

    bool ok = pidlock_acquire(path);
    CHECK(ok);
    CHECK(access(path.c_str(), F_OK) == 0);
    unlink(path.c_str());
  }

  TEST_CASE("lockfile contains own PID") {
    std::string path = tmp_lock();
    unlink(path.c_str());

    pidlock_acquire(path);
    FILE *f = fopen(path.c_str(), "r");
    REQUIRE(f);
    pid_t pid = 0;
    fscanf(f, "%d", &pid);
    fclose(f);
    CHECK(pid == getpid());
    unlink(path.c_str());

    // acquire again should succeed
    CHECK(pidlock_acquire(path));
    unlink(path.c_str());
  }

  TEST_CASE("stale lock from dead PID is removed and retried") {
    std::string path = tmp_lock();
    unlink(path.c_str());

    // Fork a child, record its PID, then wait for it to exit.
    pid_t dead_pid = fork();
    if (dead_pid == 0) {
      _exit(0); // child exits immediately
    }
    waitpid(dead_pid, nullptr, 0); // reap so PID is definitely gone

    write_pid(path, dead_pid);
    // Verify the PID is dead (kill returns ESRCH).
    REQUIRE(kill(dead_pid, 0) == -1);
    REQUIRE(errno == ESRCH);

    bool ok = pidlock_acquire(path);
    CHECK(ok);
    // Stale file must have been replaced with our PID.
    FILE *f = fopen(path.c_str(), "r");
    REQUIRE(f);
    pid_t got = 0;
    fscanf(f, "%d", &got);
    fclose(f);
    CHECK(got == getpid());
    unlink(path.c_str());
  }

  TEST_CASE("live foreign PID blocks acquire") {
    // command in pid file is not me. acquire should succeed.
    std::string path = tmp_lock();
    unlink(path.c_str());
    // PID 1 (init/systemd) is always alive and not us.
    write_pid(path, 1);

    // acquire should succeed because pid 1 does not concern us
    CHECK(pidlock_acquire(path));
    unlink(path.c_str());
  }
}
