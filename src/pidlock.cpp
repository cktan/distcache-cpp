#include "pidlock.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static std::string read_comm(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return "";
  char buf[64] = {};
  (void)fgets(buf, sizeof(buf), f);
  fclose(f);
  int n = strlen(buf);
  if (n > 0 && buf[n - 1] == '\n')
    buf[n - 1] = '\0';
  return buf;
}

static bool same_command(pid_t other_pid) {
  std::string a = read_comm(other_pid);
  std::string b = read_comm(getpid());
  return !a.empty() && a == b;
}

static bool try_acquire(const std::string &path) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd >= 0) {
    // File does not exist. Write our pid and acquire it.
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    (void)write(fd, buf, n);
    close(fd);
    return true;  // success
  }

  if (errno != EEXIST)
    return false;

  // PID lock exists!

  // Obtain pid in the file
  FILE *f = fopen(path.c_str(), "r");
  if (!f)
    return false;
  pid_t other_pid = 0;
  int rc = fscanf(f, "%d", &other_pid);
  fclose(f);
  if (rc != 1) {
    pidlock_release(path);
    return false;
  }

  // we already own this lock!
  if (other_pid == getpid()) {
    return true;
  }

  // block if another instance of the same command is running
  if (same_command(other_pid)) {
    return false;
  }

  // stale lock from a dead process
  pidlock_release(path);
  return false;
}

bool pidlock_acquire(const std::string &path) {
  if (try_acquire(path)) {
    return true;
  }
  return try_acquire(path);
}
    

void pidlock_release(const std::string &path) { unlink(path.c_str()); }
