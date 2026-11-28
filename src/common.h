// Shared declarations: config, path utilities, dialogs, viewer/editor/file ops.
#pragma once
#include <sys/stat.h>
#include <sys/types.h>

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "term.h"

// ---- platform differences
#if defined(_WIN32)
#define ST_ATIM(st) (st).st_atim
#define ST_MTIM(st) (st).st_mtim
#define TWINC_OS "Windows"
#elif defined(__APPLE__)
#define ST_ATIM(st) (st).st_atimespec
#define ST_MTIM(st) (st).st_mtimespec
#define TWINC_OS "macOS"
#else
#define ST_ATIM(st) (st).st_atim
#define ST_MTIM(st) (st).st_mtim
#define TWINC_OS "Linux"
#endif
#ifdef _WIN32
#include <vector>
int sysRun(const std::string& command, const std::string& cwd, const std::string& other);
int sysOpen(const std::string& path);  // 0 ok, 1 no association, -1 no desktop
std::vector<std::string> sysDrives();  // "C:/", "D:/", ...
std::string sysVolumeInfo(const std::string& path, std::string& fsName, unsigned long long& total,
                          unsigned long long& avail);  // returns the volume label
int sysClearAttrs(const char* path);
int sysCopyFile(const std::string& src, const std::string& dst, bool (*progress)(unsigned long long, void*),
                void* ctx);
#endif
// File hidden in the GUI file manager (Finder flag on macOS; none on Linux)
bool sysHidden(const struct stat& st);

#define TWINC_VERSION "1.2.0"

struct Config {
  bool showHidden = false;
  bool pause = false;      // wait for a key after running a command
  bool mouse = true;
  bool clock = true;
  bool extEditor = false;  // use $EDITOR instead of the built-in editor
  bool trash = true;       // F8 moves to the Trash (Shift-F8 deletes permanently)
};
extern Config cfg;

// ---- paths / formatting (util.cpp) ----
std::string joinPath(const std::string& dir, const std::string& name);
std::string normPath(const std::string& p);
bool isAbsPath(const std::string& p);
bool isRootPath(const std::string& p);  // "/" (or "C:/" on Windows)
// p with exactly one trailing '/', for prefix tests ("/" and "C:/" already end in one)
inline std::string dirSlash(const std::string& p) { return !p.empty() && p.back() == '/' ? p : p + "/"; }
std::string parentPath(const std::string& p);
std::string baseName(const std::string& p);
std::string homeDir();
std::string configDir();
std::string expandTilde(const std::string& p);
std::string resolvePath(const std::string& base, const std::string& input);
std::string displayPath(const std::string& p);
std::string shellQuote(const std::string& s);
std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::string fmtThousands(unsigned long long v);
std::string fmtSize(unsigned long long v, int width);
std::string fmtDate(time_t t);
std::string fmtTime(time_t t);
std::string fmtPerms(mode_t m);
bool isDir(const std::string& p);
bool existsL(const std::string& p);
int mkdirP(const std::string& p);
std::vector<std::string> splitMasks(const std::string& s);
bool matchMasks(const std::vector<std::string>& masks, const std::string& name);

// ---- dialogs (ui.cpp) ----
namespace ui {
extern std::function<void()> backdrop;  // draws whatever is behind a dialog (no flush)

struct BackdropGuard {
  std::function<void()> old;
  explicit BackdropGuard(std::function<void()> f) : old(backdrop) { backdrop = std::move(f); }
  ~BackdropGuard() { backdrop = old; }
};

struct LineEdit {
  std::u32string s;
  size_t pos = 0, scroll = 0;
  void set(const std::string& v);
  std::string get() const;
  bool empty() const { return s.empty(); }
  void clear() { s.clear(); pos = 0; scroll = 0; }
  bool key(int k);
  void draw(int x, int y, int w, Attr a, bool focus);
};

Attr dlgAttr(bool warn);
void frame(int x, int y, int w, int h, Attr a, const std::string& title, bool shadow = true);
void keybar(const std::vector<std::string>& labels);
int message(const std::string& title, const std::vector<std::string>& lines,
            const std::vector<std::string>& buttons, bool warn = false, int def = 0);
void error(const std::string& msg);
bool input(const std::string& title, const std::string& prompt, std::string& val,
           std::vector<std::string>* hist = nullptr);
int list(const std::string& title, const std::vector<std::string>& items, int sel = 0,
         bool hotkeys = false);
void text(const std::string& title, const std::vector<std::string>& lines);
void progress(const std::string& title, const std::vector<std::string>& lines, double f1, double f2);
}  // namespace ui

// ---- viewer / editor ----
void runViewer(const std::string& path);
void runEditor(const std::string& path);

// ---- file operations (fileops.cpp) ----
bool fo_copyMove(const std::string& srcDir, const std::vector<std::string>& names,
                 const std::string& dest, bool move);
bool fo_delete(const std::string& dir, const std::vector<std::string>& names);
bool fo_trash(const std::string& dir, const std::vector<std::string>& names);
bool sysTrash(const std::string& path, std::string& err);  // trash.mm / trash_linux.cpp
std::string sysOpenCmd();                                   // "open" / "xdg-open"
bool fo_dirSize(const std::string& p, unsigned long long& bytes, unsigned long long& files,
                unsigned long long& dirs);
