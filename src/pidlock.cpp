#include "pidlock.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

static bool try_acquire(const std::string &path, bool can_retry = true) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd >= 0) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    (void)write(fd, buf, n);
    close(fd);
    return true;
  }

  if (errno != EEXIST)
    return false;

  FILE *f = fopen(path.c_str(), "r");
  if (!f)
    return false;
  pid_t pid = 0;
  int rc = fscanf(f, "%d", &pid);
  fclose(f);
  if (rc != 1)
    return false;

  if (pid == getpid())
    return true; // our own PID (crash-recovery path)

  if (pid > 0 && kill(pid, 0) == 0)
    return false; // live owner
  if (errno != ESRCH)
    return false;

  // Stale lock — remove and retry once.
  if (!can_retry || unlink(path.c_str()) != 0)
    return false;
  return try_acquire(path, false);
}

bool pidlock_acquire(const std::string &path) { return try_acquire(path); }

void pidlock_release(const std::string &path) { unlink(path.c_str()); }
