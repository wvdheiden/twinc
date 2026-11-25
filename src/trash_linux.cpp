// Move to Trash on Linux following the freedesktop.org Trash specification:
// files on the home file system go to $XDG_DATA_HOME/Trash, files elsewhere to
// $topdir/.Trash-$uid. Each item gets a .trashinfo with its original path, so the
// desktop's file manager can restore it.
#ifndef __APPLE__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "common.h"

namespace {

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string r;
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("/-_.~", c)) {
      r += (char)c;
    } else {
      r += '%';
      r += hex[c >> 4];
      r += hex[c & 15];
    }
  }
  return r;
}

// Top directory (mount point) of the file system holding path.
std::string topDir(const std::string& path, dev_t dev) {
  std::string p = path;
  for (;;) {
    std::string up = parentPath(p);
    struct stat st;
    if (up == p || lstat(up.c_str(), &st) != 0 || st.st_dev != dev) return p;
    p = up;
  }
}

bool ensureDirs(const std::string& trash) {
  return mkdirP(trash + "/files") == 0 && mkdirP(trash + "/info") == 0;
}

}  // namespace

bool sysTrash(const std::string& path, std::string& err) {
  struct stat st, hs;
  if (lstat(path.c_str(), &st) != 0) {
    err = strerror(errno);
    return false;
  }
  const char* xd = getenv("XDG_DATA_HOME");
  std::string dataHome = (xd && *xd) ? std::string(xd) : homeDir() + "/.local/share";
  mkdirP(dataHome);
  std::string trash, infoPath;
  if (stat(dataHome.c_str(), &hs) == 0 && hs.st_dev == st.st_dev) {
    trash = dataHome + "/Trash";
    infoPath = path;  // absolute
  } else {
    std::string top = topDir(parentPath(path), st.st_dev);
    trash = joinPath(top, ".Trash-" + std::to_string(getuid()));
    infoPath = path.substr(top == "/" ? 1 : top.size() + 1);  // relative to topdir
  }
  if (!ensureDirs(trash)) {
    err = "cannot create " + displayPath(trash);
    return false;
  }
  chmod(trash.c_str(), 0700);

  // Reserve a unique name by creating the .trashinfo file exclusively.
  std::string base = baseName(path), name, info;
  int fd = -1;
  for (int i = 1; i < 10000; i++) {
    name = i == 1 ? base : base + "." + std::to_string(i);
    info = trash + "/info/" + name + ".trashinfo";
    fd = open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) {
    err = strerror(errno);
    return false;
  }
  char date[32];
  time_t now = time(nullptr);
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&now));
  std::string txt = "[Trash Info]\nPath=" + urlEncode(infoPath) + "\nDeletionDate=" + date + "\n";
  bool ok = write(fd, txt.data(), txt.size()) == (ssize_t)txt.size();
  close(fd);
  if (ok && rename(path.c_str(), (trash + "/files/" + name).c_str()) == 0) return true;
  err = ok ? strerror(errno) : "cannot write trash info";
  unlink(info.c_str());
  return false;
}
#endif
