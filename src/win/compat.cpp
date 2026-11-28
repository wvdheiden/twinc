// Windows implementations of the POSIX pieces twinc uses (see compat.h).
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <shlobj.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>

#include "compat.h"
#include "dirent.h"
#include "fnmatch.h"
#include "ftw.h"
#include "pwd.h"
#include "sys/mman.h"

std::wstring u8to16(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string u16to8(const wchar_t* s, int len) {
  if (!s) return "";
  if (len < 0) len = (int)wcslen(s);
  if (len == 0) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
  std::string r((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, len, &r[0], n, nullptr, nullptr);
  return r;
}

// "C:/x" -> L"C:\\x"; a bare drive "C:" becomes "C:\\" (the root, not the drive's cwd)
static std::wstring winPath(const char* p) {
  std::wstring w = u8to16(p ? p : "");
  for (auto& c : w)
    if (c == L'/') c = L'\\';
  if (w.size() == 2 && w[1] == L':') w += L'\\';
  return w;
}

static int setErr(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH: case ERROR_INVALID_DRIVE: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
    case ERROR_NOT_READY: errno = EIO; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
    default: errno = EINVAL;
  }
  return -1;
}

// FILETIME (100 ns since 1601) <-> timespec (since 1970)
static const int64_t EPOCH_DIFF = 116444736000000000LL;
static timespec ftToTs(FILETIME ft) {
  int64_t t = ((int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - EPOCH_DIFF;
  timespec ts;
  ts.tv_sec = (time_t)(t / 10000000);
  ts.tv_nsec = (long)(t % 10000000) * 100;
  if (ts.tv_nsec < 0) { ts.tv_nsec += 1000000000; ts.tv_sec--; }
  return ts;
}
static FILETIME tsToFt(const timespec& ts) {
  int64_t t = (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100 + EPOCH_DIFF;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)t;
  ft.dwHighDateTime = (DWORD)(t >> 32);
  return ft;
}

static bool isExeName(const std::wstring& w) {
  size_t dot = w.rfind(L'.');
  if (dot == std::wstring::npos) return false;
  std::wstring e = w.substr(dot);
  for (auto& c : e) c = (wchar_t)towlower(c);
  return e == L".exe" || e == L".bat" || e == L".cmd" || e == L".com" || e == L".ps1";
}

static void fillStat(struct twinc_stat* st, DWORD attr, FILETIME a, FILETIME m, FILETIME c, uint64_t size,
                     const std::wstring& name, bool link) {
  memset(st, 0, sizeof *st);
  st->st_attr = attr;
  if (link) st->st_mode = S_IFLNK | 0777;
  else if (attr & FILE_ATTRIBUTE_DIRECTORY) st->st_mode = S_IFDIR | 0755;
  else st->st_mode = S_IFREG | ((attr & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644) | (isExeName(name) ? 0111 : 0);
  st->st_size = (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (int64_t)size;
  st->st_atim = ftToTs(a);
  st->st_mtim = ftToTs(m);
  st->st_ctim = ftToTs(c);
  st->st_atime = st->st_atim.tv_sec;
  st->st_mtime = st->st_mtim.tv_sec;
  st->st_ctime = st->st_ctim.tv_sec;
  st->st_nlink = 1;
}

static int statImpl(const char* path, struct twinc_stat* st, bool follow) {
  std::wstring w = winPath(path);
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) return setErr(GetLastError());
  bool link = false;
  if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      FindClose(h);
      link = fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
    }
  }
  if (link && follow) {  // attributes of the target
    HANDLE h = CreateFileW(w.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return setErr(GetLastError());
    BY_HANDLE_FILE_INFORMATION bi;
    BOOL ok = GetFileInformationByHandle(h, &bi);
    CloseHandle(h);
    if (!ok) return setErr(GetLastError());
    fillStat(st, bi.dwFileAttributes, bi.ftLastAccessTime, bi.ftLastWriteTime, bi.ftCreationTime,
             (uint64_t)bi.nFileSizeHigh << 32 | bi.nFileSizeLow, w, false);
    return 0;
  }
  fillStat(st, d.dwFileAttributes, d.ftLastAccessTime, d.ftLastWriteTime, d.ftCreationTime,
           (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow, w, link);
  return 0;
}

int twinc_stat_fn(const char* path, struct twinc_stat* st) { return statImpl(path, st, true); }
int twinc_lstat_fn(const char* path, struct twinc_stat* st) { return statImpl(path, st, false); }

int twinc_fstat_fn(int fd, struct twinc_stat* st) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  BY_HANDLE_FILE_INFORMATION bi;
  if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &bi)) {
    // pipes and consoles: report a character device
    memset(st, 0, sizeof *st);
    st->st_mode = 0020000;
    return 0;
  }
  fillStat(st, bi.dwFileAttributes, bi.ftLastAccessTime, bi.ftLastWriteTime, bi.ftCreationTime,
           (uint64_t)bi.nFileSizeHigh << 32 | bi.nFileSizeLow, L"", false);
  return 0;
}

int mkdir(const char* p, int) {
  if (CreateDirectoryW(winPath(p).c_str(), nullptr)) return 0;
  return setErr(GetLastError());
}

int fchmod(int, unsigned) { return 0; }

int symlink(const char*, const char*) {
  errno = ENOSYS;
  return -1;
}

long readlink(const char* path, char* buf, size_t n) {
  HANDLE h = CreateFileW(winPath(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return setErr(GetLastError());
  wchar_t w[MAX_PATH * 4];
  DWORD len = GetFinalPathNameByHandleW(h, w, (DWORD)(sizeof w / sizeof w[0]), FILE_NAME_NORMALIZED);
  CloseHandle(h);
  if (len == 0 || len >= sizeof w / sizeof w[0]) return setErr(GetLastError());
  std::string s = u16to8(w, (int)len);
  if (s.rfind("\\\\?\\", 0) == 0) s = s.substr(4);
  for (auto& c : s)
    if (c == '\\') c = '/';
  size_t m = std::min(n, s.size());
  memcpy(buf, s.data(), m);
  return (long)m;
}

char* realpath(const char* p, char* resolved) {
  long n = readlink(p, resolved, 1023);
  if (n < 0) return nullptr;
  resolved[n] = 0;
  return resolved;
}

int utimensat(int, const char* path, const struct timespec ts[2], int) {
  HANDLE h = CreateFileW(winPath(path).c_str(), FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return setErr(GetLastError());
  FILETIME a = tsToFt(ts[0]), m = tsToFt(ts[1]);
  BOOL ok = SetFileTime(h, nullptr, ts[0].tv_nsec == UTIME_OMIT ? nullptr : &a,
                        ts[1].tv_nsec == UTIME_OMIT ? nullptr : &m);
  CloseHandle(h);
  return ok ? 0 : setErr(GetLastError());
}

unsigned getuid() { return 0; }

// ---------------------------------------------------------------- dirent

struct twinc_DIR {
  HANDLE h = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW fd;
  bool first = true;
  dirent ent;
};

DIR* opendir(const char* path) {
  std::wstring w = winPath(path);
  if (!w.empty() && w.back() != L'\\') w += L'\\';
  w += L'*';
  DIR* d = new twinc_DIR;
  d->h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &d->fd, FindExSearchNameMatch, nullptr,
                          FIND_FIRST_EX_LARGE_FETCH);
  if (d->h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    delete d;
    if (e == ERROR_FILE_NOT_FOUND) {  // empty drive root: no "." entries
      static twinc_DIR empty;
      empty.first = false;
      return &empty;
    }
    setErr(e);
    return nullptr;
  }
  return d;
}

struct dirent* readdir(DIR* d) {
  if (d->h == INVALID_HANDLE_VALUE) return nullptr;
  if (!d->first && !FindNextFileW(d->h, &d->fd)) return nullptr;
  d->first = false;
  std::string n = u16to8(d->fd.cFileName);
  size_t m = std::min(n.size(), sizeof d->ent.d_name - 1);
  memcpy(d->ent.d_name, n.data(), m);
  d->ent.d_name[m] = 0;
  DWORD a = d->fd.dwFileAttributes;
  if ((a & FILE_ATTRIBUTE_REPARSE_POINT) &&
      (d->fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || d->fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
    d->ent.d_type = DT_LNK;
  else d->ent.d_type = (a & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
  return &d->ent;
}

int closedir(DIR* d) {
  if (d->h != INVALID_HANDLE_VALUE) {
    FindClose(d->h);
    delete d;
  }
  return 0;
}

// ---------------------------------------------------------------- pwd

struct passwd* getpwuid(unsigned) {
  static std::string name, dir;
  static passwd pw;
  if (name.empty()) {
    wchar_t buf[256];
    DWORD n = 256;
    name = GetUserNameW(buf, &n) ? u16to8(buf) : "user";
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p) == S_OK) {
      dir = u16to8(p);
      CoTaskMemFree(p);
    } else {
      dir = "C:/";
    }
    for (auto& c : dir)
      if (c == '\\') c = '/';
  }
  pw.pw_name = name.c_str();
  pw.pw_dir = dir.c_str();
  return &pw;
}

// ---------------------------------------------------------------- fnmatch (*, ?, [...])

static bool fnm(const wchar_t* p, const wchar_t* s, bool fold) {
  auto eq = [fold](wchar_t a, wchar_t b) { return fold ? towlower(a) == towlower(b) : a == b; };
  for (; *p; p++, s++) {
    if (*p == L'*') {
      while (*p == L'*') p++;
      if (!*p) return true;
      for (; *s; s++)
        if (fnm(p, s, fold)) return true;
      return false;
    }
    if (!*s) return false;
    if (*p == L'?') continue;
    if (*p == L'[') {
      const wchar_t* q = p + 1;
      bool neg = *q == L'!' || *q == L'^';
      if (neg) q++;
      bool hit = false;
      for (bool firstc = true; *q && (firstc || *q != L']'); q++, firstc = false) {
        if (q[1] == L'-' && q[2] && q[2] != L']') {
          wchar_t c = fold ? (wchar_t)towlower(*s) : *s;
          wchar_t lo = fold ? (wchar_t)towlower(q[0]) : q[0], hi = fold ? (wchar_t)towlower(q[2]) : q[2];
          if (c >= lo && c <= hi) hit = true;
          q += 2;
        } else if (eq(*q, *s)) {
          hit = true;
        }
      }
      if (!*q) {  // no closing bracket: literal '['
        if (*s != L'[') return false;
        continue;
      }
      if (hit == neg) return false;
      p = q;
      continue;
    }
    if (!eq(*p, *s)) return false;
  }
  return !*s;
}

int fnmatch(const char* pattern, const char* name, int flags) {
  return fnm(u8to16(pattern).c_str(), u8to16(name).c_str(), (flags & FNM_CASEFOLD) != 0) ? 0 : FNM_NOMATCH;
}

// ---------------------------------------------------------------- nftw (depth-first, for removal)

static int walk(const std::string& p, int (*fn)(const char*, const struct twinc_stat*, int, struct FTW*), int lvl) {
  struct twinc_stat st;
  if (twinc_lstat_fn(p.c_str(), &st) != 0) return 0;
  FTW f{(int)(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1), lvl};
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = opendir(p.c_str())) {
      while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int r = walk(p + "/" + e->d_name, fn, lvl + 1);
        if (r) { closedir(d); return r; }
      }
      closedir(d);
    }
    return fn(p.c_str(), &st, FTW_DP, &f);
  }
  return fn(p.c_str(), &st, FTW_F, &f);
}

int nftw(const char* path, int (*fn)(const char*, const struct twinc_stat*, int, struct FTW*), int, int) {
  return walk(path, fn, 0);
}

// ---------------------------------------------------------------- mmap (read-only)

void* mmap(void*, size_t len, int, int, int fd, long off) {
  HANDLE f = (HANDLE)_get_osfhandle(fd);
  HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!m) { setErr(GetLastError()); return MAP_FAILED; }
  void* p = MapViewOfFile(m, FILE_MAP_READ, 0, (DWORD)off, len);
  CloseHandle(m);
  if (!p) { setErr(GetLastError()); return MAP_FAILED; }
  return p;
}

int munmap(void* addr, size_t) { return UnmapViewOfFile(addr) ? 0 : -1; }
#endif
