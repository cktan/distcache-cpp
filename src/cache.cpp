#include "cache.h"
#include "config.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static std::atomic<uint32_t> g_tmp_counter{0};

bool cache_exists(const std::string &abspath) {
  return access(abspath.c_str(), R_OK) == 0;
}

// Create parent directories recursively (like mkdir -p).
static bool mkdirs(const std::string &path) {
  for (size_t i = 1; i < path.size(); i++) {
    if (path[i] != '/')
      continue;
    std::string dir = path.substr(0, i);
    if (mkdir(dir.c_str(), 0755) < 0 && errno != EEXIST)
      return false;
  }
  return true;
}

bool cache_write(const std::string &abspath, const char *data, size_t len) {
  std::string tmp = tmp_path();

  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0)
    return false;

  bool ok = true;
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(fd, data + written, len - written);
    if (n < 0) {
      ok = false;
      break;
    }
    written += n;
  }
  close(fd);

  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }

  if (!mkdirs(abspath)) {
    unlink(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), abspath.c_str()) < 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool cache_read(const std::string &abspath, std::string &data) {
  FILE *f = fopen(abspath.c_str(), "rb");
  if (!f)
    return false;

  struct stat st;
  if (fstat(fileno(f), &st) < 0) {
    fclose(f);
    return false;
  }

  data.resize(st.st_size);
  size_t n = fread(data.data(), 1, st.st_size, f);
  fclose(f);
  if (static_cast<off_t>(n) != st.st_size)
    return false;
  return true;
}

bool cache_del(const std::string &abspath) {
  return unlink(abspath.c_str()) == 0 || errno == ENOENT;
}

std::string tmp_path() {
  uint32_t n = g_tmp_counter.fetch_add(1, std::memory_order_relaxed);
  char buf[64];
  snprintf(buf, sizeof(buf), "%u-%d.tmp", n, getpid());
  return g_config.homedir + "/tmp/" + buf;
}

// Walk a directory tree calling cb for each regular file.
static void walk(const std::string &dir, const std::string &relbase,
                 const std::function<void(const std::string &, time_t)> &cb) {
  DIR *d = opendir(dir.c_str());
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.')
      continue;

    std::string name = ent->d_name;
    std::string fpath = dir + "/" + name;
    std::string rel = relbase + "/" + name;

    struct stat st;
    if (stat(fpath.c_str(), &st) < 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      walk(fpath, rel, cb);
    } else if (S_ISREG(st.st_mode)) {
      cb(rel, st.st_mtime);
    }
  }
  closedir(d);
}

void scan_cache(const std::function<void(const std::string &, time_t)> &cb) {
  std::string cache_dir = g_config.homedir + "/cache";
  walk(cache_dir, "cache", cb);
}
