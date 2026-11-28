#include <fnmatch.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>

#include "common.h"

#ifdef _WIN32
// Windows paths are kept as "C:/dir/sub"; the root of a drive is "C:/".
static bool hasDrive(const std::string& s) { return s.size() >= 2 && isalpha((unsigned char)s[0]) && s[1] == ':'; }
bool isAbsPath(const std::string& s) { return (!s.empty() && (s[0] == '/' || s[0] == '\\')) || hasDrive(s); }
bool isRootPath(const std::string& p) { return p == "/" || (p.size() == 3 && hasDrive(p) && p[2] == '/'); }

static std::string cwdPath() {
  char buf[PATH_MAX * 3];
  std::string c = getcwd(buf, sizeof buf) ? buf : "C:/";
  for (auto& ch : c)
    if (ch == '\\') ch = '/';
  return c;
}

std::string normPath(const std::string& p) {
  std::string s = p.empty() ? cwdPath() : p;
  for (auto& ch : s)
    if (ch == '\\') ch = '/';
  std::string drive;
  if (hasDrive(s)) {
    drive = s.substr(0, 2);
    s = s.substr(2);
    if (s.empty() || s[0] != '/') s = "/" + s;  // "C:foo" is taken as "C:/foo"
  } else if (s[0] == '/') {
    drive = cwdPath().substr(0, 2);
  } else {
    std::string c = cwdPath();
    drive = c.substr(0, 2);
    s = c.substr(2) + "/" + s;
  }
  drive[0] = (char)toupper((unsigned char)drive[0]);
  std::vector<std::string> parts;
  std::string cur;
  for (size_t j = 0; j <= s.size(); j++) {
    if (j == s.size() || s[j] == '/') {
      if (cur == "..") {
        if (!parts.empty()) parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur += s[j];
    }
  }
  std::string r = drive;
  for (auto& x : parts) r += "/" + x;
  return parts.empty() ? drive + "/" : r;
}

std::string joinPath(const std::string& dir, const std::string& name) {
  if (hasDrive(name)) return name;
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

std::string parentPath(const std::string& p) {
  if (p.empty() || isRootPath(p)) return p.empty() ? "/" : p;
  auto pos = p.rfind('/');
  if (pos == std::string::npos) return p;
  if (pos == 2 && hasDrive(p)) return p.substr(0, 3);
  if (pos == 0) return "/";
  return p.substr(0, pos);
}

std::string baseName(const std::string& p) {
  if (isRootPath(p)) return p;
  auto pos = p.rfind('/');
  return pos == std::string::npos ? p : p.substr(pos + 1);
}
#else
bool isAbsPath(const std::string& s) { return !s.empty() && s[0] == '/'; }
bool isRootPath(const std::string& p) { return p == "/"; }

std::string joinPath(const std::string& dir, const std::string& name) {
  if (!name.empty() && name[0] == '/') return name;
  if (dir.empty() || dir == "/") return "/" + name;
  return dir + "/" + name;
}

std::string normPath(const std::string& p) {
  std::string s = p.empty() ? "/" : p;
  if (s[0] != '/') {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf)) s = std::string(buf) + "/" + s;
    else s = "/" + s;
  }
  std::vector<std::string> parts;
  std::string cur;
  for (size_t j = 0; j <= s.size(); j++) {
    if (j == s.size() || s[j] == '/') {
      if (cur == "..") {
        if (!parts.empty()) parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur += s[j];
    }
  }
  std::string r;
  for (auto& x : parts) r += "/" + x;
  return r.empty() ? "/" : r;
}

std::string parentPath(const std::string& p) {
  if (p == "/" || p.empty()) return "/";
  auto pos = p.rfind('/');
  if (pos == 0 || pos == std::string::npos) return "/";
  return p.substr(0, pos);
}

std::string baseName(const std::string& p) {
  if (p == "/") return "/";
  auto pos = p.rfind('/');
  return pos == std::string::npos ? p : p.substr(pos + 1);
}

#endif

std::string homeDir() {
#ifdef _WIN32
  return getpwuid(0)->pw_dir;
#endif
  const char* h = getenv("HOME");
  if (h && *h) return h;
  struct passwd* pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/";
}

std::string configDir() {
#ifdef _WIN32
  const char* x = getenv("APPDATA");
  std::string base = (x && *x) ? normPath(x) : homeDir() + "/AppData/Roaming";
#else
  const char* x = getenv("XDG_CONFIG_HOME");
  std::string base = (x && *x) ? std::string(x) : homeDir() + "/.config";
#endif
  std::string dir = base + "/twinc";
  // one-time migration from the old name
  struct stat st;
  std::string old = base + "/ncmac";
  if (lstat(dir.c_str(), &st) != 0 && lstat(old.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    rename(old.c_str(), dir.c_str());
  std::string oc = dir + "/ncmac.conf", nc = dir + "/twinc.conf";
  if (lstat(nc.c_str(), &st) != 0 && lstat(oc.c_str(), &st) == 0) rename(oc.c_str(), nc.c_str());
  return dir;
}

std::string expandTilde(const std::string& p) {
  if (p == "~") return homeDir();
  if (p.size() >= 2 && p[0] == '~' && p[1] == '/') return homeDir() + p.substr(1);
  return p;
}

std::string resolvePath(const std::string& base, const std::string& input) {
  std::string s = expandTilde(trim(input));
  if (s.empty()) return base;
  if (!isAbsPath(s)) s = joinPath(base, s);
  return normPath(s);
}

std::string displayPath(const std::string& p) {
#ifdef _WIN32
  std::string r = p;
  for (auto& c : r)
    if (c == '/') c = '\\';
  return r;
#endif
  std::string h = homeDir();
  if (h != "/" && (p == h || (p.size() > h.size() && p.compare(0, h.size(), h) == 0 && p[h.size()] == '/')))
    return "~" + p.substr(h.size());
  return p;
}

std::string shellQuote(const std::string& s) {
#ifdef _WIN32
  // cmd.exe: double quotes around anything with spaces or special characters
  bool plain = !s.empty() && s.find_first_of(" \t&()[]{}^=;!'+,`~%\"") == std::string::npos;
  return plain ? s : "\"" + s + "\"";
#endif
  if (s.empty()) return "''";
  bool safe = true;
  for (unsigned char c : s) {
    if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/' || c == '+' || c == '@' ||
          c == '%' || c == ':' || c == ',' || c >= 0x80)) {
      safe = false;
      break;
    }
  }
  if (safe) return s;
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string toLower(const std::string& s) {
  std::string r = s;
  for (auto& c : r) c = (char)tolower((unsigned char)c);
  return r;
}

std::string fmtThousands(unsigned long long v) {
  std::string s = std::to_string(v);
  std::string r;
  int n = 0;
  for (int i = (int)s.size() - 1; i >= 0; i--) {
    r.insert(r.begin(), s[i]);
    if (++n % 3 == 0 && i > 0) r.insert(r.begin(), '.');
  }
  return r;
}

std::string fmtSize(unsigned long long v, int width) {
  std::string s = std::to_string(v);
  if ((int)s.size() <= width) return s;
  const char* units = "KMGTP";
  double d = (double)v;
  for (int u = 0; u < 5; u++) {
    d /= 1024.0;
    s = std::to_string((unsigned long long)(d + 0.5)) + units[u];
    if ((int)s.size() <= width) return s;
  }
  return s;
}

std::string fmtDate(time_t t) {
  char b[32];
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(b, sizeof b, "%d-%m-%y", &tmv);
  return b;
}

std::string fmtTime(time_t t) {
  char b[32];
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(b, sizeof b, "%H:%M", &tmv);
  return b;
}

std::string fmtPerms(mode_t m) {
  std::string r(10, '-');
  if (S_ISDIR(m)) r[0] = 'd';
  else if (S_ISLNK(m)) r[0] = 'l';
  else if (S_ISCHR(m)) r[0] = 'c';
  else if (S_ISBLK(m)) r[0] = 'b';
  else if (S_ISFIFO(m)) r[0] = 'p';
  else if (S_ISSOCK(m)) r[0] = 's';
  const char* rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++)
    if (m & (1 << (8 - i))) r[i + 1] = rwx[i];
  if (m & S_ISUID) r[3] = (m & S_IXUSR) ? 's' : 'S';
  if (m & S_ISGID) r[6] = (m & S_IXGRP) ? 's' : 'S';
  if (m & S_ISVTX) r[9] = (m & S_IXOTH) ? 't' : 'T';
  return r;
}

bool isDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool existsL(const std::string& p) {
  struct stat st;
  return lstat(p.c_str(), &st) == 0;
}

int mkdirP(const std::string& p) {
  std::string cur;
  std::string n = normPath(p);
  size_t i = isRootPath(n.substr(0, 3)) && n.size() > 3 ? 3 : 1;
  while (true) {
    size_t j = n.find('/', i);
    cur = n.substr(0, j);
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return -1;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return isDir(n) ? 0 : -1;
}

std::vector<std::string> splitMasks(const std::string& s) {
  std::vector<std::string> r;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == ',' || c == ' ') {
      if (!cur.empty()) r.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) r.push_back(cur);
  for (auto& m : r)
    if (m == "*.*") m = "*";  // DOS habit
  if (r.empty()) r.push_back("*");
  return r;
}

bool matchMasks(const std::vector<std::string>& masks, const std::string& name) {
  for (auto& m : masks)
    if (fnmatch(m.c_str(), name.c_str(), FNM_CASEFOLD) == 0) return true;
  return false;
}

bool sysHidden(const struct stat& st) {
#if defined(_WIN32)
  return (st.st_attr & 0x6) != 0;  // FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
#elif defined(__APPLE__)
  return (st.st_flags & UF_HIDDEN) != 0;
#else
  (void)st;
  return false;
#endif
}

std::string sysOpenCmd() {
#ifdef __APPLE__
  return "/usr/bin/open";
#else
  return "xdg-open";
#endif
}
