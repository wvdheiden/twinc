#include "panel.h"

#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <sys/mount.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <strings.h>

static std::string extOf(const std::string& n) {
  auto p = n.rfind('.');
  if (p == std::string::npos || p == 0) return "";
  return n.substr(p + 1);
}

static int cmpName(const std::string& a, const std::string& b) {
  int r = strcasecmp(a.c_str(), b.c_str());
  if (r == 0) r = a.compare(b);
  return r;
}

bool Panel::load(const std::string& focusName) {
  if (inArchive()) {
    loadArchive(focusName);
    return true;
  }
  std::set<std::string> marks;
  for (auto& x : e)
    if (x.marked) marks.insert(x.name);
  std::string curName = focusName;
  if (curName.empty() && cur >= 0 && cur < (int)e.size()) curName = e[cur].name;
  int oldCur = cur;

  DIR* d = opendir(path.c_str());
  if (!d) return false;
  e.clear();
  if (path != "/") {
    Entry up;
    up.name = "..";
    up.dir = true;
    e.push_back(up);
  }
  while (dirent* de = readdir(d)) {
    std::string n = de->d_name;
    if (n == "." || n == "..") continue;
    if (!cfg.showHidden && n[0] == '.') continue;
    Entry x;
    x.name = n;
    x.hidden = n[0] == '.';
    std::string fp = joinPath(path, n);
    struct stat st;
    if (lstat(fp.c_str(), &st) == 0) {
      if (!cfg.showHidden && (st.st_flags & UF_HIDDEN)) continue;
      x.hidden = n[0] == '.' || (st.st_flags & UF_HIDDEN);
      x.mode = st.st_mode;
      x.size = (unsigned long long)st.st_size;
      x.mtime = st.st_mtime;
      if (S_ISLNK(st.st_mode)) {
        x.link = true;
        struct stat t;
        if (stat(fp.c_str(), &t) == 0) {
          x.dir = S_ISDIR(t.st_mode);
          if (!x.dir) x.size = (unsigned long long)t.st_size;
        } else {
          x.broken = true;
        }
      } else {
        x.dir = S_ISDIR(st.st_mode);
      }
      x.exec = !x.dir && (st.st_mode & 0111) && !S_ISLNK(st.st_mode) && S_ISREG(st.st_mode);
    }
    x.marked = marks.count(n) > 0;
    e.push_back(x);
  }
  closedir(d);
  sortEntries();
  struct stat ds;
  if (stat(path.c_str(), &ds) == 0) dirMtime = ds.st_mtimespec;
  cur = -1;
  if (!curName.empty())
    for (size_t i = 0; i < e.size(); i++)
      if (e[i].name == curName) { cur = (int)i; break; }
  if (cur < 0) cur = oldCur;
  clampCursor();
  return true;
}

bool Panel::openArchive(const std::string& file, std::string* err) {
  std::vector<ArcEntry> idx;
  std::string er;
  if (!arcList(file, idx, er)) {
    if (err) *err = er;
    return false;
  }
  arcFile = normPath(file);
  arcIndex = std::move(idx);
  archiveCd("");
  return true;
}

void Panel::archiveCd(const std::string& dir, const std::string& focusName) {
  arcDir = dir;
  path = arcFile + (dir.empty() ? "" : "/" + dir);
  e.clear();
  cur = 0;
  top = 0;
  loadArchive(focusName);
  if (focusName.empty()) cur = 0;
  clampCursor();
}

void Panel::loadArchive(const std::string& focusName) {
  std::set<std::string> marks;
  for (auto& x : e)
    if (x.marked) marks.insert(x.name);
  std::string curName = focusName;
  if (curName.empty() && cur >= 0 && cur < (int)e.size()) curName = e[cur].name;
  int oldCur = cur;
  std::map<std::string, Entry> m;
  std::string pre = arcDir.empty() ? "" : arcDir + "/";
  for (auto& a : arcIndex) {
    if (!pre.empty() && a.path.compare(0, pre.size(), pre) != 0) continue;
    std::string rest = a.path.substr(pre.size());
    if (rest.empty()) continue;
    size_t sl = rest.find('/');
    std::string name = rest.substr(0, sl);
    if (!cfg.showHidden && (name[0] == '.' || name == "__MACOSX")) continue;
    Entry& en = m[name];
    en.name = name;
    en.hidden = name[0] == '.' || name == "__MACOSX";
    if (sl == std::string::npos) {
      en.dir = en.dir || a.dir;
      en.link = a.link;
      en.size = a.size;
      en.mtime = a.mtime;
      en.mode = a.perm | (a.dir ? S_IFDIR : S_IFREG);
    } else {
      en.dir = true;
      en.mode = 0755 | S_IFDIR;
      if (!en.mtime) en.mtime = a.mtime;
    }
  }
  e.clear();
  Entry up;
  up.name = "..";
  up.dir = true;
  e.push_back(up);
  for (auto& kv : m) {
    Entry x = kv.second;
    x.marked = marks.count(x.name) > 0;
    e.push_back(x);
  }
  sortEntries();
  cur = -1;
  if (!curName.empty())
    for (size_t i = 0; i < e.size(); i++)
      if (e[i].name == curName) { cur = (int)i; break; }
  if (cur < 0) cur = oldCur;
  clampCursor();
}

bool Panel::changedOnDisk() const {
  if (inArchive()) return false;
  struct stat ds;
  if (stat(path.c_str(), &ds) != 0) return true;
  return ds.st_mtimespec.tv_sec != dirMtime.tv_sec || ds.st_mtimespec.tv_nsec != dirMtime.tv_nsec;
}

bool Panel::chdirTo(const std::string& p, std::string* err) {
  std::string n = normPath(p);
  if (inArchive() && (n == arcFile || n.rfind(arcFile + "/", 0) == 0)) {
    std::string rel = n == arcFile ? "" : n.substr(arcFile.size() + 1);
    std::string focusName;
    if (arcDir.size() > rel.size() && (rel.empty() || arcDir.rfind(rel + "/", 0) == 0)) {
      std::string rest = arcDir.substr(rel.empty() ? 0 : rel.size() + 1);
      focusName = rest.substr(0, rest.find('/'));
    }
    archiveCd(rel, focusName);
    return true;
  }
  DIR* d = opendir(n.c_str());
  if (!d) {
    if (err) *err = std::string(strerror(errno));
    return false;
  }
  closedir(d);
  std::string old = path, focusName;
  if (old.size() > n.size() && old.compare(0, n.size(), n) == 0 && (n == "/" || old[n.size()] == '/')) {
    std::string rest = old.substr(n == "/" ? 1 : n.size() + 1);
    focusName = rest.substr(0, rest.find('/'));
  }
  path = n;
  arcFile.clear();
  arcDir.clear();
  arcIndex.clear();
  e.clear();
  cur = 0;
  top = 0;
  load(focusName);
  if (focusName.empty()) cur = 0;
  clampCursor();
  return true;
}

void Panel::sortEntries() {
  auto cmp = [this](const Entry& a, const Entry& b) {
    if (a.name == "..") return b.name != "..";
    if (b.name == "..") return false;
    if (sort == Sort::Unsorted) return false;
    if (a.dir != b.dir) return a.dir;
    int r = 0;
    switch (sort) {
      case Sort::Name: r = cmpName(a.name, b.name); break;
      case Sort::Ext:
        r = strcasecmp(extOf(a.name).c_str(), extOf(b.name).c_str());
        if (r == 0) r = cmpName(a.name, b.name);
        break;
      case Sort::Time:
        r = a.mtime > b.mtime ? -1 : a.mtime < b.mtime ? 1 : 0;
        if (r == 0) r = cmpName(a.name, b.name);
        break;
      case Sort::Size:
        r = a.size > b.size ? -1 : a.size < b.size ? 1 : 0;
        if (r == 0) r = cmpName(a.name, b.name);
        break;
      default: break;
    }
    return reverse ? r > 0 : r < 0;
  };
  std::stable_sort(e.begin(), e.end(), cmp);
}

bool Panel::focus(const std::string& name) {
  for (size_t i = 0; i < e.size(); i++)
    if (e[i].name == name) { cur = (int)i; clampCursor(); return true; }
  return false;
}

const Entry* Panel::current() const {
  if (cur < 0 || cur >= (int)e.size()) return nullptr;
  return &e[cur];
}

std::vector<std::string> Panel::selection() const {
  std::vector<std::string> r;
  for (auto& x : e)
    if (x.marked && x.name != "..") r.push_back(x.name);
  if (r.empty()) {
    const Entry* c = current();
    if (c && c->name != "..") r.push_back(c->name);
  }
  return r;
}

int Panel::markedCount(unsigned long long* bytes) const {
  int n = 0;
  unsigned long long b = 0;
  for (auto& x : e)
    if (x.marked) { n++; if (!x.dir) b += x.size; }
  if (bytes) *bytes = b;
  return n;
}

int Panel::listRows() const { return std::max(1, gh - 5); }

int Panel::briefCols() const {
  int iw = gw - 2;
  if (iw >= 3 * 12) return 3;
  if (iw >= 2 * 10) return 2;
  return 1;
}

int Panel::pageSize() const { return view == View::Brief ? listRows() * briefCols() : listRows(); }

void Panel::clampCursor() {
  int n = (int)e.size();
  if (n == 0) { cur = 0; top = 0; return; }
  cur = std::max(0, std::min(cur, n - 1));
  int rows = listRows();
  if (view == View::Brief) {
    int cols = briefCols();
    if (cur < top) top = (cur / rows) * rows;
    if (cur >= top + rows * cols) top = (cur / rows - cols + 1) * rows;
    if (top < 0) top = 0;
  } else {
    if (cur < top) top = cur;
    if (cur >= top + rows) top = cur - rows + 1;
    if (top > std::max(0, n - rows)) top = std::max(0, n - rows);
  }
}

static std::string sizeField(const Entry& en) {
  if (en.name == "..") return "►UP--DIR◄";
  if (en.dir) return "►SUB-DIR◄";
  return fmtSize(en.size, 9);
}

static Attr entryAttr(const Entry& en, bool isCur) {
  if (isCur) return en.marked ? mk(C_YELLOW, C_CYAN) : mk(C_BLACK, C_CYAN);
  if (en.marked) return mk(C_YELLOW, C_BLUE);
  if (en.hidden) return mk(en.dir ? C_LGRAY : C_CYAN, C_BLUE);  // dimmed
  if (en.dir) return mk(C_WHITE, C_BLUE);
  return mk(C_LCYAN, C_BLUE);
}

void Panel::draw(int x, int y, int w, int h, bool active) {
  gx = x; gy = y; gw = w; gh = h;
  Attr fr = mk(C_LCYAN, C_BLUE);
  Attr hdr = mk(C_YELLOW, C_BLUE);
  scr.rect(x, y, w, h, fr);
  scr.box(x, y, w, h, fr, true);
  if (w < 8 || h < 6) return;
  int iw = w - 2, rows = listRows();
  int sepY = y + h - 3;
  scr.set(x, sepY, "╟", fr);
  scr.fill(x + 1, sepY, iw, fr, "─");
  scr.set(x + w - 1, sepY, "╢", fr);
  clampCursor();

  auto divider = [&](int dx) {
    scr.set(dx, y, "╤", fr);
    for (int r = y + 1; r < sepY; r++) scr.set(dx, r, "│", fr);
    scr.set(dx, sepY, "┴", fr);
  };

  if (view == View::Full) {
    const int sizeW = 9, dateW = 8, timeW = 5;
    int nameW = iw - (sizeW + dateW + timeW + 3);
    bool cols = nameW >= 8;
    if (!cols) nameW = iw;
    int dx[3] = {0, 0, 0};
    if (cols) {
      dx[0] = x + 1 + nameW;
      dx[1] = dx[0] + 1 + sizeW;
      dx[2] = dx[1] + 1 + dateW;
      for (int d : dx) divider(d);
      scr.put(dx[0] + 1, y + 1, u8center("Size", sizeW), hdr);
      scr.put(dx[1] + 1, y + 1, u8center("Date", dateW), hdr);
      scr.put(dx[2] + 1, y + 1, u8center("Time", timeW), hdr);
    }
    scr.put(x + 1, y + 1, u8center("Name", nameW), hdr);
    for (int r = 0; r < rows; r++) {
      int i = top + r;
      if (i >= (int)e.size()) break;
      int ry = y + 2 + r;
      const Entry& en = e[i];
      bool isCur = active && i == cur;
      Attr na = entryAttr(en, isCur);
      if (isCur) {
        scr.fill(x + 1, ry, iw, na);
        if (cols)
          for (int d : dx) scr.set(d, ry, "│", na);
      }
      scr.put(x + 1, ry, u8fit(en.name, nameW), na);
      if (cols) {
        scr.put(dx[0] + 1, ry, u8rjust(sizeField(en), sizeW), na);
        if (en.name != "..") {
          scr.put(dx[1] + 1, ry, fmtDate(en.mtime), na);
          scr.put(dx[2] + 1, ry, fmtTime(en.mtime), na);
        }
      }
    }
  } else {
    int nc = briefCols();
    int cw = (iw - (nc - 1)) / nc;
    std::vector<int> colX(nc), colW(nc);
    for (int c = 0; c < nc; c++) {
      colX[c] = x + 1 + c * (cw + 1);
      colW[c] = (c == nc - 1) ? (x + 1 + iw) - colX[c] : cw;
      if (c < nc - 1) divider(colX[c] + cw);
      scr.put(colX[c], y + 1, u8center("Name", colW[c]), hdr);
    }
    for (int c = 0; c < nc; c++)
      for (int r = 0; r < rows; r++) {
        int i = top + c * rows + r;
        if (i >= (int)e.size()) break;
        const Entry& en = e[i];
        bool isCur = active && i == cur;
        Attr na = entryAttr(en, isCur);
        scr.put(colX[c], y + 2 + r, u8fit(en.name, colW[c]), na);
      }
  }

  // title (path)
  std::string t = " " + u8truncLeft(displayPath(path), iw - 12) + " ";
  scr.put(x + (w - u8width(t)) / 2, y, t, active ? mk(C_BLACK, C_CYAN) : fr);

  // footer
  int fy = y + h - 2;
  unsigned long long bytes = 0;
  int mc = markedCount(&bytes);
  if (mc > 0) {
    std::string s = fmtThousands(bytes) + " bytes in " + std::to_string(mc) + " selected file" +
                    (mc > 1 ? "s" : "");
    s = u8trunc(s, iw);
    scr.put(x + 1 + (iw - u8width(s)) / 2, fy, s, mk(C_YELLOW, C_BLUE));
  } else if (const Entry* en = current()) {
    std::string right = u8rjust(sizeField(*en), 9);
    if (en->name != "..") right += " " + fmtDate(en->mtime) + " " + fmtTime(en->mtime);
    std::string nm = en->name;
    if (en->link) {
      char buf[PATH_MAX];
      ssize_t n = readlink(joinPath(path, en->name).c_str(), buf, sizeof buf);
      if (n > 0) nm += " -> " + std::string(buf, (size_t)n);
    }
    int rw = u8width(right);
    if (rw + 8 > iw) right.clear(), rw = 0;
    scr.put(x + 1, fy, u8fit(nm, iw - rw - (rw ? 1 : 0)), fr);
    if (rw) scr.put(x + 1 + iw - rw, fy, right, fr);
  }
}

void Panel::drawInfo(int x, int y, int w, int h, const Panel& o) {
  gx = x; gy = y; gw = w; gh = h;
  Attr fr = mk(C_LCYAN, C_BLUE);
  Attr hi = mk(C_YELLOW, C_BLUE);
  scr.rect(x, y, w, h, fr);
  scr.box(x, y, w, h, fr, true);
  std::string t = " Info ";
  scr.put(x + (w - u8width(t)) / 2, y, t, fr);
  int iw = w - 4;
  int r = y + 2;
  auto line = [&](const std::string& s, Attr a) {
    if (r < y + h - 1) scr.put(x + 2, r, u8center(s, iw), a);
    r++;
  };
  line("ncmac version " NCMAC_VERSION, hi);
  line("", fr);
  char host[256] = "";
  gethostname(host, sizeof host);
  struct passwd* pw = getpwuid(getuid());
  line(std::string(pw ? pw->pw_name : "?") + "@" + host, fr);
  line("", fr);
  struct statfs sf;
  if (statfs(o.realDir().c_str(), &sf) == 0) {
    unsigned long long avail = (unsigned long long)sf.f_bavail * sf.f_bsize;
    unsigned long long total = (unsigned long long)sf.f_blocks * sf.f_bsize;
    line(fmtThousands(avail) + " bytes free", fr);
    line("of " + fmtThousands(total) + " bytes", fr);
    line("on " + std::string(sf.f_mntonname) + " (" + sf.f_fstypename + ")", fr);
  }
  line("", fr);
  unsigned long long bytes = 0;
  int files = 0, dirs = 0;
  for (auto& en : o.e) {
    if (en.name == "..") continue;
    if (en.dir) dirs++;
    else { files++; bytes += en.size; }
  }
  line(std::to_string(files) + " files use " + fmtThousands(bytes) + " bytes", fr);
  line(std::to_string(dirs) + " directories", fr);
  line("in " + displayPath(o.path), fr);
  if (const Entry* en = o.current()) {
    if (en->name != "..") {
      line("", fr);
      line(en->name, hi);
      std::string fp = joinPath(o.path, en->name);
      struct stat st;
      if (lstat(fp.c_str(), &st) == 0) {
        struct passwd* op = getpwuid(st.st_uid);
        line(fmtPerms(st.st_mode) + "  " + (op ? op->pw_name : std::to_string(st.st_uid)), fr);
        if (!en->dir) line(fmtThousands(en->size) + " bytes", fr);
        char b[64];
        struct tm tmv;
        localtime_r(&st.st_mtime, &tmv);
        strftime(b, sizeof b, "%d-%m-%Y %H:%M:%S", &tmv);
        line(std::string("Modified ") + b, fr);
      }
    }
  }
}

int Panel::hit(int mx, int my) const {
  int rows = listRows();
  int r = my - (gy + 2);
  if (r < 0 || r >= rows) return -1;
  if (mx <= gx || mx >= gx + gw - 1) return -1;
  int idx;
  if (view == View::Full) {
    idx = top + r;
  } else {
    int nc = briefCols();
    int cw = (gw - 2 - (nc - 1)) / nc;
    int c = (mx - gx - 1) / (cw + 1);
    if (c >= nc) c = nc - 1;
    idx = top + c * rows + r;
  }
  return idx < (int)e.size() ? idx : -1;
}
