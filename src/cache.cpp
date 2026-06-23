#include "cache.h"
#include "ring.h"
#include "unique.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

// Read entire file at abspath into data. Returns false on any error.
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

// Recursively walk dir, calling cb(relpath, mtime) for each regular file.
static void walk(const std::string &dir, const std::string &relbase,
                 const std::function<void(const std::string &, time_t)> &cb) {
  DIR *d = opendir(dir.c_str());
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    // Skip . / .. and any hidden files; cache paths never start with '.'.
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

// Map the current errno to an rc_t error, using msg as the error message.
static rc_t errno_to_rc(const std::string &msg) {
  if (errno == ENOENT)
    return rc_t::err(rc_errcode_t::not_found, msg);
  if (errno == EACCES || errno == EPERM)
    return rc_t::err(rc_errcode_t::permission, msg);
  return rc_t::err(rc_errcode_t::io, msg);
}

// Initialize cache rooted at homedir; keylock is shared with the lru.
cache_t::cache_t(keylock_t &keylock, std::string homedir)
    : homedir_(std::move(homedir)),
      lru_(
          keylock,
          [](const std::string &s) { return ring_compute_pos(s.c_str()); },
          homedir_) {}

// Resolve relpath to an absolute filesystem path under homedir.
std::string cache_t::abspath(const relpath_t &relpath) const {
  return homedir_ + "/" + relpath.str();
}

// Return true if relpath exists and is readable.
bool cache_t::exists(const relpath_t &relpath) const {
  return access(abspath(relpath).c_str(), R_OK) == 0;
}

// Open relpath O_RDONLY, touch lru, return fd in out.
rc_t cache_t::read(const relpath_t &relpath, fd_t &out) {
  int fd = ::open(abspath(relpath).c_str(), O_RDONLY);
  if (fd < 0)
    return errno_to_rc(strerror(errno));
  out = fd_t(fd);
  lru_.touch(relpath);
  return {};
}

// Atomically rename src to dest, creating dest's parent dirs; updates lru.
rc_t cache_t::move(const relpath_t &src_relpath,
                   const relpath_t &dest_relpath) {
  rc_t rc = mkdirs(dest_relpath);
  if (!rc.ok())
    return rc;
  if (rename(abspath(src_relpath).c_str(), abspath(dest_relpath).c_str()) < 0)
    return errno_to_rc(strerror(errno));
  lru_.put(dest_relpath, time(nullptr));
  return {};
}

// Unlink the file; ignore ENOENT (idempotent). Update lru regardless.
void cache_t::drop(const relpath_t &relpath) {
  unlink(abspath(relpath).c_str());
  lru_.drop(relpath);
}

// Generate a unique relpath under tmp/ for use as a staging file.
relpath_t cache_t::tmp_relpath() {
  uint64_t n = unique_get();
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu-%d.tmp", (unsigned long long)n, getpid());
  return "tmp/" + std::string(buf);
}

// Create all parent directories of relpath under homedir.
rc_t cache_t::mkdirs(const relpath_t &relpath) {
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(abspath(relpath)).parent_path(), ec);
  if (ec) {
    return rc_t::err(rc_errcode_t::io, "mkdirs failed");
  }
  return {};
}

// Walk the cache/ subtree, calling cb(relpath, mtime) for each file.
void cache_t::scan_cache(
    const std::function<void(const std::string &, time_t)> &cb) {
  walk(homedir_ + "/cache", "cache", cb);
}

// Bulk-insert (relpath, mtime) pairs into the lru at startup.
void cache_t::batch_put(
    const std::vector<std::pair<std::string, time_t>> &batch) {
  lru_.batch_put(batch);
}

// Evict least-recently-used entries if disk pressure warrants it.
void cache_t::maybe_evict(
    const std::function<void(const std::string &)> &del_cb) {
  lru_.maybe_evict(del_cb);
}

// Iterate all lru entries, calling cb(relpath, mtime, relpath_pos) for each.
void cache_t::scan(
    const std::function<void(const std::string &relpath, time_t mtime,
                             uint32_t relpath_pos)> &cb) {
  lru_.scan(cb);
}

// Return the lru-recorded mtime for relpath, or 0 if not tracked.
time_t cache_t::get_mtime(const std::string &relpath) {
  return lru_.get_mtime(relpath_t{relpath});
}

// Number of entries currently tracked in the lru.
size_t cache_t::size() { return lru_.size(); }

// Disk usage as an integer percentage (0–100).
int cache_t::disk_usepct() { return lru_.disk_usepct(); }

// Disk usage in gibibytes.
double cache_t::disk_used_gib() { return lru_.disk_used_gib(); }

// True if a disk stat error was recorded during the last eviction check.
bool cache_t::disk_error() { return lru_.disk_error(); }
