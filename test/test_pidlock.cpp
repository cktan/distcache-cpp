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
  /**
   * @test acquire creates lockfile
   * @brief Tests that successfully acquiring a PID lock creates the lockfile on
   * disk.
   *
   * Steps:
   * 1. Construct a temporary lockfile path.
   * 2. Remove any pre-existing lockfile at that path.
   * 3. Call pidlock_acquire.
   * 4. Verify that acquisition succeeds (true) and the file exists on disk.
   * 5. Clean up the lockfile.
   */
  TEST_CASE("acquire creates lockfile") {
    std::string path = tmp_lock();
    unlink(path.c_str());

    bool ok = pidlock_acquire(path);
    CHECK(ok);
    CHECK(access(path.c_str(), F_OK) == 0);
    unlink(path.c_str());
  }

  /**
   * @test lockfile contains own PID
   * @brief Tests that the lockfile created by pidlock_acquire contains the
   * current process's PID written inside.
   *
   * Steps:
   * 1. Construct a temporary lockfile path and ensure it is deleted.
   * 2. Call pidlock_acquire.
   * 3. Open the created lockfile and read the PID stored inside.
   * 4. Verify that the stored PID matches the current process ID (getpid()).
   * 5. Verify that attempting to acquire the lock again (when already held by
   * us) succeeds.
   * 6. Clean up the lockfile.
   */
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

  /**
   * @test stale lock from dead PID is removed and retried
   * @brief Tests that if a lockfile exists but contains the PID of a dead
   * process, pidlock_acquire detects it as stale, deletes it, and successfully
   * acquires the lock.
   *
   * Steps:
   * 1. Construct a temporary lockfile path and ensure it is deleted.
   * 2. Fork a child process, record its PID, and wait for it to exit
   * immediately to guarantee a dead PID.
   * 3. Write the dead child's PID to the lockfile.
   * 4. Verify the PID is indeed dead (sending signal 0 returns ESRCH).
   * 5. Call pidlock_acquire.
   * 6. Verify that lock acquisition succeeds, and that the file now contains
   * the current process ID.
   * 7. Clean up the lockfile.
   */
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

  /**
   * @test live foreign PID blocks acquire
   * @brief Tests that a lock file containing a live foreign PID running a
   * different command does not block lock acquisition.
   *
   * Steps:
   * 1. Construct a temporary lockfile path and delete it.
   * 2. Write PID 1 (which is always alive and running a different command like
   * systemd/init) to the lockfile.
   * 3. Call pidlock_acquire.
   * 4. Verify that lock acquisition succeeds since the command of PID 1 is not
   * the current program.
   * 5. Clean up the lockfile.
   */
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
