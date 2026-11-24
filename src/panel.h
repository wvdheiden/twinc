#pragma once
#include <sys/stat.h>

#include "archive.h"
#include "common.h"

struct Entry {
  std::string name;
  bool dir = false, link = false, exec = false, broken = false, marked = false;
  unsigned long long size = 0;
  time_t mtime = 0;
  mode_t mode = 0;
};

enum class Sort { Name, Ext, Time, Size, Unsorted };
enum class View { Brief, Full, Info };

struct Panel {
  std::string path;
  std::vector<Entry> e;
  int cur = 0, top = 0;
  Sort sort = Sort::Name;
  bool reverse = false;
  View view = View::Full, prevView = View::Full;
  bool visible = true;
  struct timespec dirMtime {};
  int gx = 0, gy = 0, gw = 40, gh = 20;  // geometry of last draw

  // archive browsing: path == arcFile [+ "/" + arcDir]
  std::string arcFile, arcDir;
  std::vector<ArcEntry> arcIndex;
  bool inArchive() const { return !arcFile.empty(); }
  std::string realDir() const { return inArchive() ? parentPath(arcFile) : path; }
  bool openArchive(const std::string& file, std::string* err = nullptr);
  void archiveCd(const std::string& dir, const std::string& focusName = "");
  void loadArchive(const std::string& focusName);

  bool load(const std::string& focusName = "");
  bool chdirTo(const std::string& p, std::string* err = nullptr);
  void sortEntries();
  bool focus(const std::string& name);
  const Entry* current() const;
  std::vector<std::string> selection() const;
  int markedCount(unsigned long long* bytes = nullptr) const;
  int listRows() const;
  int briefCols() const;
  int pageSize() const;
  void clampCursor();
  void draw(int x, int y, int w, int h, bool active);
  void drawInfo(int x, int y, int w, int h, const Panel& other);
  int hit(int mx, int my) const;
  bool changedOnDisk() const;
  bool isList() const { return visible && view != View::Info; }
};
