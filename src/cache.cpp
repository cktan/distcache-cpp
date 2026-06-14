#include "cache.h"
#include "unique.h"
#include <cstdio>
#include <cstring>
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

bool cache_del(const std::string &abspath) {
  return unlink(abspath.c_str()) == 0 || errno == ENOENT;
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

cache_t::cache_t(const std::string &homedir) : homedir_(homedir) {}

std::string cache_t::abspath(const std::string &relpath) const {
  return homedir_ + "/" + relpath;
}

bool cache_t::exists(const std::string &relpath) const {
  return access(abspath(relpath).c_str(), R_OK) == 0;
}

rc_t cache_t::read(const std::string &relpath, fd_t &out) {
  int fd = ::open(abspath(relpath).c_str(), O_RDONLY);
  if (fd < 0)
    return errno_to_rc(strerror(errno));
  out = fd_t(fd);
  return {};
}

rc_t cache_t::write(const std::string &relpath, fd_t &out) {
  rc_t rc = mkdirs(relpath);
  if (!rc.ok()) {
    return rc;
  }
  int fd = ::open(abspath(relpath).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return errno_to_rc(strerror(errno));
  out = fd_t(fd);
  return {};
}

rc_t cache_t::move(const std::string &src_relpath,
                   const std::string &dest_relpath) {
  rc_t rc = mkdirs(dest_relpath);
  if (!rc.ok())
    return rc;
  if (rename(abspath(src_relpath).c_str(), abspath(dest_relpath).c_str()) < 0)
    return errno_to_rc(strerror(errno));
  return {};
}

rc_t cache_t::drop(const std::string &relpath) {
  if (unlink(abspath(relpath).c_str()) < 0 && errno != ENOENT)
    return errno_to_rc(strerror(errno));
  return {};
}

std::string cache_t::tmp_path() {
  uint64_t n = unique_get();
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu-%d.tmp", (unsigned long long)n, getpid());
  return homedir_ + "/tmp/" + buf;
}

rc_t cache_t::mkdirs(const std::string &relpath) {
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
  walk(homedir_ + "/cache", "cache", cb);
}
