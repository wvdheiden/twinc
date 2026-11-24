#include "archive.h"

#include <ftw.h>
#include <unistd.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common.h"

// libarchive ships with macOS but its headers are not in the SDK: declare the stable C API we use.
extern "C" {
struct archive;
struct archive_entry;
struct archive* archive_read_new(void);
int archive_read_support_filter_all(struct archive*);
int archive_read_support_format_all(struct archive*);
int archive_read_open_filename(struct archive*, const char*, size_t);
int archive_read_next_header(struct archive*, struct archive_entry**);
ssize_t archive_read_data(struct archive*, void*, size_t);
int archive_read_free(struct archive*);
const char* archive_error_string(struct archive*);
const char* archive_entry_pathname(struct archive_entry*);
const char* archive_entry_pathname_utf8(struct archive_entry*);
int64_t archive_entry_size(struct archive_entry*);
time_t archive_entry_mtime(struct archive_entry*);
mode_t archive_entry_filetype(struct archive_entry*);
mode_t archive_entry_perm(struct archive_entry*);
const char* archive_entry_symlink(struct archive_entry*);
}

static const int ARC_OK = 0, ARC_EOF = 1, ARC_WARN = -20;

bool arcLooksLike(const std::string& name) {
  static const char* exts[] = {".zip", ".jar", ".war", ".ear", ".apk", ".ipa", ".xpi", ".cbz", ".tar", ".tgz",
                               ".tbz", ".tbz2", ".txz", ".tzst", ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst",
                               ".tar.lz", ".tar.lzma", ".tar.z", ".7z", ".rar", ".cbr", ".iso", ".cpio",
                               ".xar", ".cab", ".lha", ".lzh", ".deb", ".rpm"};
  std::string l = toLower(name);
  for (const char* e : exts) {
    size_t n = strlen(e);
    if (l.size() > n && l.compare(l.size() - n, n, e) == 0) return true;
  }
  return false;
}

static std::string normEntry(const std::string& p) {
  std::string r;
  std::string cur;
  for (size_t i = 0; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '/') {
      if (cur == "..") return "";  // never allow escaping the archive
      if (!cur.empty() && cur != ".") r += (r.empty() ? "" : "/") + cur;
      cur.clear();
    } else {
      cur += p[i];
    }
  }
  return r;
}

ArcReader::~ArcReader() {
  if (a) archive_read_free((struct archive*)a);
}

bool ArcReader::open(const std::string& file) {
  struct archive* ar = archive_read_new();
  a = ar;
  archive_read_support_filter_all(ar);
  archive_read_support_format_all(ar);
  if (archive_read_open_filename(ar, file.c_str(), 65536) != ARC_OK) {
    const char* e = archive_error_string(ar);
    err = e ? e : "cannot open archive";
    return false;
  }
  return true;
}

bool ArcReader::next(ArcEntry& e) {
  struct archive* ar = (struct archive*)a;
  for (;;) {
    struct archive_entry* en = nullptr;
    int r = archive_read_next_header(ar, &en);
    if (r == ARC_EOF) return false;
    if (r < ARC_WARN) {
      const char* s = archive_error_string(ar);
      err = s ? s : "archive error";
      return false;
    }
    cur = en;
    const char* p = archive_entry_pathname_utf8(en);
    if (!p) p = archive_entry_pathname(en);
    if (!p) continue;
    e = ArcEntry();
    e.path = normEntry(p);
    if (e.path.empty()) continue;
    mode_t t = archive_entry_filetype(en);
    e.dir = (t & S_IFMT) == S_IFDIR;
    e.link = (t & S_IFMT) == S_IFLNK;
    e.size = e.dir ? 0 : (unsigned long long)std::max<int64_t>(0, archive_entry_size(en));
    e.mtime = archive_entry_mtime(en);
    e.perm = archive_entry_perm(en) & 07777;
    if (e.perm == 0) e.perm = e.dir ? 0755 : 0644;
    return true;
  }
}

long ArcReader::read(void* buf, size_t n) { return (long)archive_read_data((struct archive*)a, buf, n); }

std::string ArcReader::symlink() const {
  const char* s = cur ? archive_entry_symlink((struct archive_entry*)cur) : nullptr;
  return s ? s : "";
}

std::string ArcReader::error() const {
  if (!err.empty()) return err;
  const char* s = a ? archive_error_string((struct archive*)a) : nullptr;
  return s ? s : "unknown error";
}

bool arcList(const std::string& file, std::vector<ArcEntry>& out, std::string& err) {
  ArcReader r;
  out.clear();
  if (!r.open(file)) { err = r.error(); return false; }
  ArcEntry e;
  while (r.next(e)) out.push_back(e);
  std::string e2 = r.error();
  if (out.empty() && e2 != "unknown error") { err = e2; return false; }
  if (out.empty()) { err = "empty or unsupported archive"; return false; }
  return true;
}

static int rmCb(const char* p, const struct stat*, int flag, struct FTW*) {
  if (flag == FTW_DP || flag == FTW_D) rmdir(p);
  else unlink(p);
  return 0;
}

void removeTree(const std::string& p) { nftw(p.c_str(), rmCb, 32, FTW_DEPTH | FTW_PHYS); }
