// Read-only archive browsing via the system libarchive (zip, tar.*, 7z, rar, iso, ...).
#pragma once
#include <sys/types.h>

#include <ctime>
#include <string>
#include <vector>

struct ArcEntry {
  std::string path;  // normalized, no leading/trailing '/'
  unsigned long long size = 0;
  time_t mtime = 0;
  bool dir = false, link = false;
  mode_t perm = 0644;
};

bool arcLooksLike(const std::string& name);  // known archive extension (Enter opens it)
bool arcList(const std::string& file, std::vector<ArcEntry>& out, std::string& err);

class ArcReader {
 public:
  ~ArcReader();
  bool open(const std::string& file);
  bool next(ArcEntry& e);           // false at end or on error (see error())
  long read(void* buf, size_t n);   // 0 = end of entry, <0 = error
  std::string symlink() const;
  std::string error() const;

 private:
  void* a = nullptr;
  void* cur = nullptr;
  std::string err;
};

// Extract the entries `names` (relative to `inDir` inside the archive) to `dest`.
bool fo_arcExtract(const std::string& arc, const std::string& inDir, const std::vector<std::string>& names,
                   const std::string& dest, const std::vector<ArcEntry>* index);
void removeTree(const std::string& p);
