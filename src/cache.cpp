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

static rc_t errno_to_rc(const std::string &msg) {
  if (errno == ENOENT)
    return rc_t::err(rc_errcode_t::not_found, msg);
  if (errno == EACCES || errno == EPERM)
    return rc_t::err(rc_errcode_t::permission, msg);
  return rc_t::err(rc_errcode_t::io, msg);
}

cache_t::cache_t(keylock_t &keylock)
    : lru_("./", keylock, ring_t::compute_id) {}

std::string cache_t::abspath(const relpath_t &relpath) const {
  return std::filesystem::absolute("./" + relpath.str());
}

bool cache_t::exists(const relpath_t &relpath) const {
  return access(abspath(relpath).c_str(), R_OK) == 0;
}

rc_t cache_t::read(const relpath_t &relpath, fd_t &out) {
  int fd = ::open(abspath(relpath).c_str(), O_RDONLY);
  if (fd < 0)
    return errno_to_rc(strerror(errno));
  out = fd_t(fd);
  lru_.touch(relpath);
  return {};
}

rc_t cache_t::write(const relpath_t &relpath, fd_t &out) {
  rc_t rc = mkdirs(relpath);
  if (!rc.ok())
    return rc;
  int fd = ::open(abspath(relpath).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return errno_to_rc(strerror(errno));
  out = fd_t(fd);
  lru_.put(relpath, time(nullptr));
  return {};
}

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

// TODO: drop() should be void. nobody cares if we can drop it.
rc_t cache_t::drop(const relpath_t &relpath) {
  if (unlink(abspath(relpath).c_str()) < 0 && errno != ENOENT)
    return errno_to_rc(strerror(errno));
  lru_.drop(relpath);
  return {};
}

relpath_t cache_t::tmp_relpath() {
  uint64_t n = unique_get();
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu-%d.tmp", (unsigned long long)n, getpid());
  return "tmp/" + std::string(buf);
}

rc_t cache_t::mkdirs(const relpath_t &relpath) {
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(abspath(relpath)).parent_path(), ec);
  if (ec) {
    return rc_t::err(rc_errcode_t::io, "mkdirs failed");
  }
  return {};
}

void cache_t::scan_cache(
    const std::function<void(const std::string &, time_t)> &cb) {
  walk("./cache", "cache", cb);
}

void cache_t::batch_put(
    const std::vector<std::pair<std::string, time_t>> &batch) {
  lru_.batch_put(batch);
}

void cache_t::maybe_evict(
    const std::function<void(const std::string &)> &del_cb) {
  lru_.maybe_evict(del_cb);
}

void cache_t::scan(
    const std::function<void(const std::string &relpath, time_t mtime,
                             uint32_t relpath_id)> &cb) {
  lru_.scan(cb);
}

time_t cache_t::get_mtime(const std::string &relpath) {
  return lru_.get_mtime(relpath_t{relpath});
}

size_t cache_t::size() { return lru_.size(); }
int cache_t::disk_usepct() { return lru_.disk_usepct(); }
double cache_t::disk_used_gib() { return lru_.disk_used_gib(); }
bool cache_t::disk_error() { return lru_.disk_error(); }
