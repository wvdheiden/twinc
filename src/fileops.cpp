// Copy / move / delete with progress, overwrite confirmation and abort (Esc).
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>

#include "archive.h"
#include "common.h"

#include <set>

namespace {

using clk = std::chrono::steady_clock;

struct Op {
  bool overAll = false, skipAll = false, cancel = false, delAll = false;
  unsigned long long totBytes = 0, doneBytes = 0, totFiles = 0, doneFiles = 0, curSize = 0, curDone = 0;
  std::string title, verb, cur, dst;
  clk::time_point last{};

  void show(bool force = false) {
    auto now = clk::now();
    if (!force && now - last < std::chrono::milliseconds(80)) return;
    last = now;
    std::vector<std::string> lines{verb + " " + displayPath(cur)};
    if (!dst.empty()) lines.push_back("to " + displayPath(dst));
    double f1 = curSize ? (double)curDone / (double)curSize : -1;
    double f2 = totBytes ? (double)(doneBytes + curDone) / (double)totBytes
                         : (totFiles ? (double)doneFiles / (double)totFiles : 0);
    ui::progress(title, lines, f1, f2);
  }

  bool poll() {
    int k = term::readKey(0);
    if (k == K_ESC) {
      int r = ui::message(title, {"Abort the operation?"}, {"Yes", "No"}, true);
      if (r == 0) cancel = true;
      last = clk::time_point{};
      if (!cancel) show(true);
    }
    return cancel;
  }
};

void scan(const std::string& p, Op& op) {
  struct stat st;
  if (lstat(p.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    DIR* d = opendir(p.c_str());
    if (!d) return;
    while (dirent* de = readdir(d)) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      scan(joinPath(p, de->d_name), op);
    }
    closedir(d);
  } else {
    op.totFiles++;
    if (S_ISREG(st.st_mode)) op.totBytes += (unsigned long long)st.st_size;
  }
}

// returns 1 = skip, -1 = cancel
int errAsk(Op& op, const std::string& msg, int err) {
  int r = ui::message("Error", {msg, strerror(err)}, {"Skip", "Cancel"}, true);
  op.last = clk::time_point{};
  if (r == 0) return 1;
  op.cancel = true;
  return -1;
}

std::string stamp(const struct stat& st) {
  return fmtThousands((unsigned long long)st.st_size) + " bytes, " + fmtDate(st.st_mtime) + " " +
         fmtTime(st.st_mtime);
}

// 0 = overwrite, 1 = skip, -1 = cancel
int askOverwrite(Op& op, const std::string& dst, const struct stat& s, const struct stat& d) {
  if (op.overAll) return 0;
  if (op.skipAll) return 1;
  int r = ui::message("Warning",
                      {"The following file already exists:", displayPath(dst), "",
                       "New:      " + stamp(s), "Existing: " + stamp(d)},
                      {"Overwrite", "All", "Skip", "Never", "Cancel"}, true);
  op.last = clk::time_point{};
  switch (r) {
    case 0: return 0;
    case 1: op.overAll = true; return 0;
    case 2: return 1;
    case 3: op.skipAll = true; return 1;
  }
  op.cancel = true;
  return -1;
}

int cfCallback(int what, int stage, copyfile_state_t state, const char*, const char*, void* ctx) {
  Op* op = (Op*)ctx;
  if (stage == COPYFILE_ERR) return what == COPYFILE_COPY_DATA ? COPYFILE_QUIT : COPYFILE_CONTINUE;
  if (what == COPYFILE_COPY_DATA && stage == COPYFILE_PROGRESS) {
    off_t c = 0;
    copyfile_state_get(state, COPYFILE_STATE_COPIED, &c);
    op->curDone = (unsigned long long)c;
    op->show();
    if (op->poll()) return COPYFILE_QUIT;
  }
  return COPYFILE_CONTINUE;
}

void setTimes(const std::string& p, const struct stat& st) {
  struct timespec ts[2] = {st.st_atimespec, st.st_mtimespec};
  utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW);
}

int copyRec(const std::string& src, const std::string& dst, Op& op);

int copyReg(const std::string& src, const std::string& dst, const struct stat& st, Op& op) {
  op.cur = src;
  op.dst = dst;
  op.curSize = (unsigned long long)st.st_size;
  op.curDone = 0;
  op.show();
  if (op.poll()) return -1;
  copyfile_state_t s = copyfile_state_alloc();
  copyfile_state_set(s, COPYFILE_STATE_STATUS_CB, (const void*)&cfCallback);
  copyfile_state_set(s, COPYFILE_STATE_STATUS_CTX, (const void*)&op);
  int r = copyfile(src.c_str(), dst.c_str(), s, COPYFILE_CLONE);
  int err = errno;
  copyfile_state_free(s);
  op.curSize = op.curDone = 0;
  if (r < 0) {
    unlink(dst.c_str());
    if (op.cancel) return -1;
    return errAsk(op, "Cannot copy " + displayPath(src), err);
  }
  setTimes(dst, st);
  op.doneBytes += (unsigned long long)st.st_size;
  op.doneFiles++;
  return 0;
}

int copyRec(const std::string& src, const std::string& dst, Op& op) {
  if (op.cancel) return -1;
  struct stat st, dt;
  if (lstat(src.c_str(), &st) != 0) return errAsk(op, "Cannot read " + displayPath(src), errno);
  bool dEx = lstat(dst.c_str(), &dt) == 0;
  if (S_ISDIR(st.st_mode)) {
    if (dEx && !S_ISDIR(dt.st_mode))
      return errAsk(op, "Cannot overwrite file with directory: " + displayPath(dst), EEXIST);
    if (!dEx && mkdir(dst.c_str(), 0700) != 0)
      return errAsk(op, "Cannot create directory " + displayPath(dst), errno);
    DIR* d = opendir(src.c_str());
    if (!d) return errAsk(op, "Cannot read directory " + displayPath(src), errno);
    std::vector<std::string> kids;
    while (dirent* de = readdir(d))
      if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) kids.push_back(de->d_name);
    closedir(d);
    int res = 0;
    for (auto& k : kids) {
      int r = copyRec(joinPath(src, k), joinPath(dst, k), op);
      if (r < 0) return -1;
      if (r > 0) res = 1;
    }
    copyfile(src.c_str(), dst.c_str(), nullptr, COPYFILE_METADATA);
    setTimes(dst, st);
    return res;
  }
  if (dEx) {
    if (dt.st_dev == st.st_dev && dt.st_ino == st.st_ino)
      return errAsk(op, "Cannot copy a file onto itself: " + displayPath(dst), EINVAL);
    int a = askOverwrite(op, dst, st, dt);
    if (a != 0) return a;
    if (S_ISDIR(dt.st_mode)) return errAsk(op, "Cannot overwrite directory " + displayPath(dst), EISDIR);
    if (unlink(dst.c_str()) != 0) return errAsk(op, "Cannot overwrite " + displayPath(dst), errno);
  }
  if (S_ISLNK(st.st_mode)) {
    char buf[PATH_MAX];
    ssize_t n = readlink(src.c_str(), buf, sizeof buf);
    if (n < 0) return errAsk(op, "Cannot read link " + displayPath(src), errno);
    if (symlink(std::string(buf, (size_t)n).c_str(), dst.c_str()) != 0)
      return errAsk(op, "Cannot create link " + displayPath(dst), errno);
    op.doneFiles++;
    return 0;
  }
  if (S_ISREG(st.st_mode)) return copyReg(src, dst, st, op);
  return errAsk(op, "Cannot copy special file " + displayPath(src), ENOTSUP);
}

int removeRec(const std::string& p, Op& op, bool count) {
  if (op.cancel) return -1;
  struct stat st;
  if (lstat(p.c_str(), &st) != 0) return errAsk(op, "Cannot delete " + displayPath(p), errno);
  if (S_ISDIR(st.st_mode)) {
    DIR* d = opendir(p.c_str());
    if (!d) return errAsk(op, "Cannot read directory " + displayPath(p), errno);
    std::vector<std::string> kids;
    while (dirent* de = readdir(d))
      if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) kids.push_back(de->d_name);
    closedir(d);
    int res = 0;
    for (auto& k : kids) {
      int r = removeRec(joinPath(p, k), op, count);
      if (r < 0) return -1;
      if (r > 0) res = 1;
    }
    if (res == 0 && rmdir(p.c_str()) != 0) {
      if (!(errno == EPERM && lchflags(p.c_str(), 0) == 0 && rmdir(p.c_str()) == 0))
        return errAsk(op, "Cannot delete directory " + displayPath(p), errno);
    }
    return res;
  }
  op.cur = p;
  op.dst.clear();
  op.show();
  if (op.poll()) return -1;
  if (unlink(p.c_str()) != 0) {
    if (!(errno == EPERM && lchflags(p.c_str(), 0) == 0 && unlink(p.c_str()) == 0))
      return errAsk(op, "Cannot delete " + displayPath(p), errno);
  }
  if (count) {
    op.doneFiles++;
    if (S_ISREG(st.st_mode)) op.doneBytes += (unsigned long long)st.st_size;
  }
  return 0;
}

bool dirNonEmpty(const std::string& p) {
  DIR* d = opendir(p.c_str());
  if (!d) return false;
  bool any = false;
  while (dirent* de = readdir(d))
    if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { any = true; break; }
  closedir(d);
  return any;
}

}  // namespace

bool fo_copyMove(const std::string& srcDir, const std::vector<std::string>& names, const std::string& dest,
                 bool move) {
  Op op;
  op.title = move ? "Move" : "Copy";
  op.verb = move ? "Moving" : "Copying";
  bool destIsDir = isDir(dest);
  if (names.size() > 1 && !destIsDir) {
    if (existsL(dest)) {
      ui::error("Target is not a directory: " + displayPath(dest));
      return false;
    }
    if (mkdirP(dest) != 0) {
      ui::error("Cannot create directory " + displayPath(dest) + ": " + strerror(errno));
      return false;
    }
    destIsDir = true;
  }
  for (auto& n : names) scan(joinPath(srcDir, n), op);
  op.cur = joinPath(srcDir, names.empty() ? "" : names[0]);
  op.show(true);

  for (auto& n : names) {
    if (op.cancel) break;
    std::string src = joinPath(srcDir, n);
    std::string dst = destIsDir ? joinPath(dest, n) : dest;
    struct stat ss, dt;
    if (lstat(src.c_str(), &ss) != 0) {
      if (errAsk(op, "Cannot read " + displayPath(src), errno) < 0) break;
      continue;
    }
    if (S_ISDIR(ss.st_mode) && (dst == src || dst.compare(0, src.size() + 1, src + "/") == 0)) {
      ui::error("Cannot " + std::string(move ? "move" : "copy") + " a directory into itself");
      continue;
    }
    bool dEx = lstat(dst.c_str(), &dt) == 0;
    bool same = dEx && dt.st_dev == ss.st_dev && dt.st_ino == ss.st_ino;
    if (!move) {
      if (same) {
        ui::error("Cannot copy a file onto itself");
        continue;
      }
      if (copyRec(src, dst, op) < 0) break;
      continue;
    }
    // ---- move ----
    if (same) {  // e.g. only the case of the name changes
      if (dst != src && rename(src.c_str(), dst.c_str()) != 0)
        if (errAsk(op, "Cannot rename " + displayPath(src), errno) < 0) break;
      continue;
    }
    bool viaCopy = false;
    if (dEx) {
      if (S_ISDIR(dt.st_mode) && S_ISDIR(ss.st_mode)) {
        viaCopy = true;  // merge directories
      } else {
        int a = askOverwrite(op, dst, ss, dt);
        if (a < 0) break;
        if (a > 0) continue;
        if (S_ISDIR(dt.st_mode)) {
          if (errAsk(op, "Cannot overwrite directory " + displayPath(dst), EISDIR) < 0) break;
          continue;
        }
        unlink(dst.c_str());
      }
    }
    if (!viaCopy) {
      op.cur = src;
      op.dst = dst;
      op.show();
      if (rename(src.c_str(), dst.c_str()) == 0) {
        op.doneFiles++;
        if (S_ISREG(ss.st_mode)) op.doneBytes += (unsigned long long)ss.st_size;
        continue;
      }
      if (errno != EXDEV) {
        if (errAsk(op, "Cannot move " + displayPath(src), errno) < 0) break;
        continue;
      }
    }
    int r = copyRec(src, dst, op);
    if (r < 0) break;
    if (r == 0 && removeRec(src, op, false) < 0) break;
  }
  return !op.cancel;
}

bool fo_delete(const std::string& dir, const std::vector<std::string>& names) {
  Op op;
  op.title = "Delete";
  op.verb = "Deleting";
  for (auto& n : names) scan(joinPath(dir, n), op);
  for (auto& n : names) {
    if (op.cancel) break;
    std::string p = joinPath(dir, n);
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode) && !op.delAll && dirNonEmpty(p)) {
      int r = ui::message("Delete",
                          {"The following directory is not empty:", displayPath(p),
                           "Delete it including all its contents?"},
                          {"Delete", "All", "Skip", "Cancel"}, true);
      if (r == 1) op.delAll = true;
      else if (r == 2) continue;
      else if (r != 0) break;
    }
    if (removeRec(p, op, true) < 0) break;
  }
  return !op.cancel;
}

bool fo_trash(const std::string& dir, const std::vector<std::string>& names) {
  Op op;
  op.title = "Trash";
  op.verb = "Moving to Trash:";
  op.totFiles = names.size();
  for (auto& n : names) {
    if (op.cancel) break;
    std::string p = joinPath(dir, n);
    op.cur = p;
    op.dst.clear();
    op.show();
    if (op.poll()) break;
    std::string err;
    if (macTrash(p, err)) {
      op.doneFiles++;
      continue;
    }
    int r = ui::message("Trash", {"Cannot move to the Trash:", displayPath(p), err, "", "Delete it permanently?"},
                        {"Delete", "Skip", "Cancel"}, true);
    op.last = clk::time_point{};
    if (r == 0) {
      if (removeRec(p, op, true) < 0) break;
    } else if (r != 1) {
      break;
    }
  }
  return !op.cancel;
}

bool fo_arcExtract(const std::string& arc, const std::string& inDir, const std::vector<std::string>& names,
                   const std::string& dest, const std::vector<ArcEntry>* index) {
  Op op;
  op.title = "Copy from archive";
  op.verb = "Extracting";
  std::set<std::string> sel(names.begin(), names.end());
  std::string pre = inDir.empty() ? "" : inDir + "/";
  auto relOf = [&](const std::string& p, std::string& rel, std::string& top) {
    if (!pre.empty() && p.compare(0, pre.size(), pre) != 0) return false;
    rel = p.substr(pre.size());
    if (rel.empty()) return false;
    top = rel.substr(0, rel.find('/'));
    return sel.count(top) > 0;
  };
  if (index) {
    for (auto& e : *index) {
      std::string rel, top;
      if (!e.dir && relOf(e.path, rel, top)) { op.totFiles++; op.totBytes += e.size; }
    }
  }
  std::string destN = normPath(dest);
  bool destIsDir = isDir(destN);
  if (names.size() > 1 && !destIsDir) {
    if (existsL(destN) || mkdirP(destN) != 0) {
      ui::error("Cannot create directory " + displayPath(destN));
      return false;
    }
    destIsDir = true;
  }
  ArcReader r;
  if (!r.open(arc)) {
    ui::error("Cannot open " + displayPath(arc) + ": " + r.error());
    return false;
  }
  op.cur = arc;
  op.show(true);
  std::vector<std::pair<std::string, time_t>> dirTimes;
  std::vector<char> buf(256 * 1024);
  ArcEntry e;
  while (!op.cancel && r.next(e)) {
    std::string rel, top;
    if (!relOf(e.path, rel, top)) continue;
    std::string target = destIsDir ? joinPath(destN, rel)
                                   : (rel == top ? destN : joinPath(destN, rel.substr(top.size() + 1)));
    target = normPath(target);
    if (destN != "/" && target != destN && target.compare(0, destN.size() + 1, destN + "/") != 0) continue;
    op.cur = arc + "/" + e.path;
    op.dst = target;
    if (e.dir) {
      mkdirP(target);
      dirTimes.push_back({target, e.mtime});
      continue;
    }
    mkdirP(parentPath(target));
    struct stat dt;
    if (lstat(target.c_str(), &dt) == 0) {
      if (S_ISDIR(dt.st_mode)) {
        if (errAsk(op, "Cannot overwrite directory " + displayPath(target), EISDIR) < 0) break;
        continue;
      }
      struct stat ss;
      memset(&ss, 0, sizeof ss);
      ss.st_size = (off_t)e.size;
      ss.st_mtimespec.tv_sec = e.mtime;
      int a = askOverwrite(op, target, ss, dt);
      if (a < 0) break;
      if (a > 0) continue;
      unlink(target.c_str());
    }
    if (e.link) {
      if (symlink(r.symlink().c_str(), target.c_str()) != 0 &&
          errAsk(op, "Cannot create link " + displayPath(target), errno) < 0)
        break;
      op.doneFiles++;
      continue;
    }
    int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (e.perm & 0777) | 0600);
    if (fd < 0) {
      if (errAsk(op, "Cannot create " + displayPath(target), errno) < 0) break;
      continue;
    }
    op.curSize = e.size;
    op.curDone = 0;
    op.show();
    bool ok = true;
    std::string why;
    for (;;) {
      long n = r.read(buf.data(), buf.size());
      if (n == 0) break;
      if (n < 0) { ok = false; why = r.error(); break; }
      long off = 0;
      while (off < n) {
        ssize_t w = write(fd, buf.data() + off, (size_t)(n - off));
        if (w < 0) { if (errno == EINTR) continue; ok = false; why = strerror(errno); break; }
        off += w;
      }
      if (!ok) break;
      op.curDone += (unsigned long long)n;
      op.show();
      if (op.poll()) { ok = false; break; }
    }
    close(fd);
    op.curSize = op.curDone = 0;
    if (!ok) {
      unlink(target.c_str());
      if (op.cancel) break;
      int q = ui::message("Error", {"Cannot extract " + e.path, why}, {"Skip", "Cancel"}, true);
      op.last = clk::time_point{};
      if (q != 0) { op.cancel = true; break; }
      continue;
    }
    struct timespec ts[2] = {{e.mtime, 0}, {e.mtime, 0}};
    utimensat(AT_FDCWD, target.c_str(), ts, 0);
    op.doneBytes += e.size;
    op.doneFiles++;
  }
  for (auto it = dirTimes.rbegin(); it != dirTimes.rend(); ++it) {
    struct timespec ts[2] = {{it->second, 0}, {it->second, 0}};
    utimensat(AT_FDCWD, it->first.c_str(), ts, 0);
  }
  return !op.cancel;
}

bool fo_dirSize(const std::string& p, unsigned long long& bytes, unsigned long long& files,
                unsigned long long& dirs) {
  struct stat st;
  if (lstat(p.c_str(), &st) != 0) return false;
  if (!S_ISDIR(st.st_mode)) {
    files++;
    bytes += (unsigned long long)st.st_size;
    return true;
  }
  dirs++;
  DIR* d = opendir(p.c_str());
  if (!d) return false;
  while (dirent* de = readdir(d)) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    fo_dirSize(joinPath(p, de->d_name), bytes, files, dirs);
  }
  closedir(d);
  return true;
}
