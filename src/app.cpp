// Main application: panels, command line, menus, key handling.
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <strings.h>

#include "archive.h"
#include "panel.h"

Config cfg;

namespace {

enum Action {
  A_NONE = 0, A_HELP, A_USERMENU, A_VIEW, A_EDIT, A_COPY, A_MOVE, A_MKDIR, A_DELETE, A_SELECT,
  A_UNSELECT, A_INVERT, A_QUIT, A_NEWFILE, A_RENAME, A_FIND, A_HISTORY, A_SWAP, A_PANELS,
  A_COMPARE, A_TREE, A_EDITMENU, A_FINDER, A_ROWS, A_DIRSIZE, A_OPT_HIDDEN, A_OPT_PAUSE, A_OPT_MOUSE,
  A_OPT_CLOCK, A_OPT_EXTEDIT, A_SAVE, A_DELPERM, A_OPT_TRASH,
  A_PANEL = 1000  // + panel*100 + PanelOp
};
enum PanelOp { P_BRIEF = 1, P_FULL, P_INFO, P_TOGGLE, P_SNAME, P_SEXT, P_STIME, P_SSIZE, P_SUNS, P_REV, P_REREAD, P_DRIVE };

struct MenuItem {
  std::string label, key;
  int action = 0;
  std::function<bool()> checked;
};
struct Menu {
  std::string title;
  std::vector<MenuItem> items;
  int x = 0;
};

// Option+letter on a US/Dutch Mac keyboard without "Use Option as Meta"
uint32_t macOptLetter(uint32_t cp) {
  static const struct { uint32_t cp; char l; } map[] = {
      {0xE5, 'a'}, {0x222B, 'b'}, {0xE7, 'c'}, {0x2202, 'd'}, {0x192, 'f'}, {0xA9, 'g'},
      {0x2D9, 'h'}, {0x2206, 'j'}, {0x2DA, 'k'}, {0xAC, 'l'}, {0xB5, 'm'}, {0xF8, 'o'},
      {0x3C0, 'p'}, {0x153, 'q'}, {0xAE, 'r'}, {0xDF, 's'}, {0x2020, 't'}, {0x221A, 'v'},
      {0x2211, 'w'}, {0x2248, 'x'}, {0xA5, 'y'}, {0x3A9, 'z'}};
  for (auto& m : map)
    if (m.cp == cp) return (uint32_t)m.l;
  return 0;
}

const std::vector<std::string> kMainKeys = {"Help", "Menu", "View", "Edit", "Copy",
                                            "RenMov", "Mkdir", "Delete", "PullDn", "Quit"};

struct App {
  Panel P[2];
  int act = 0;
  bool running = true;
  ui::LineEdit cmd;
  std::vector<std::string> hist;
  int histPos = -1;
  bool qsActive = false;
  std::string qs;
  std::string prevDir;
  int origW = 0, origH = 0;
  bool resizedTerm = false;
  std::chrono::steady_clock::time_point lastClick;
  int lastClickIdx = -1, lastClickPanel = -1;
  std::vector<std::string> mkdirHist, findHist, copyHist;

  // ------------------------------------------------------------ drawing

  std::string prompt() const { return displayPath(P[act].path) + ">"; }

  void drawAll() {
    scr.clear(mk(C_DEFAULT, C_DEFAULT));
    scr.hideCursor();
    int W = scr.w, H = scr.h;
    int ph = std::max(6, H - 2);
    int lw = W / 2;
    int xs[2] = {0, lw}, ws[2] = {lw, W - lw};
    for (int i = 0; i < 2; i++) {
      if (!P[i].visible) continue;
      if (P[i].view == View::Info) P[i].drawInfo(xs[i], 0, ws[i], ph, P[1 - i]);
      else P[i].draw(xs[i], 0, ws[i], ph, i == act);
    }
    if (cfg.clock) {
      time_t t = time(nullptr);
      std::string c = fmtTime(t);
      scr.put(W - (int)c.size(), 0, c, mk(C_BLACK, C_CYAN));
    }
    std::string pr = prompt();
    if (u8width(pr) > W / 2) pr = u8truncLeft(pr, W / 2);
    int pw = u8width(pr);
    Attr ca = mk(C_DEFAULT, C_DEFAULT);
    scr.put(0, H - 2, pr, ca);
    cmd.draw(pw, H - 2, W - pw, ca, !qsActive);
    ui::keybar(kMainKeys);
    if (qsActive && P[act].isList()) {
      Panel& p = P[act];
      int bw = std::min(p.gw - 4, 30);
      int bx = p.gx + 2, by = p.gy + p.gh - 3;
      Attr a = mk(C_BLACK, C_CYAN);
      scr.rect(bx, by, bw, 3, a);
      scr.box(bx, by, bw, 3, a, false);
      scr.put(bx + 1, by + 1, " Search: ", a);
      std::string t = u8truncLeft(qs, bw - 12);
      scr.put(bx + 10, by + 1, t, mk(C_WHITE, C_BLACK));
      scr.showCursor(bx + 10 + u8width(t), by + 1);
    }
  }

  // ------------------------------------------------------------ helpers

  Panel& cp() { return P[act]; }
  Panel& op() { return P[1 - act]; }

  void reloadAll() {
    for (auto& p : P) {
      if (!p.load()) {
        // directory vanished: walk up until something is readable
        std::string d = p.path;
        while (d != "/" && !isDir(d)) d = parentPath(d);
        p.chdirTo(d);
      }
    }
  }

  void goDir(int i, const std::string& path) {
    std::string err, old = P[i].path;
    if (!P[i].chdirTo(path, &err)) {
      ui::error("Cannot open " + displayPath(path) + ": " + err);
      return;
    }
    if (i == act) prevDir = old;
  }

  void addHistory(const std::string& c) {
    auto it = std::find(hist.begin(), hist.end(), c);
    if (it != hist.end()) hist.erase(it);
    hist.push_back(c);
    if (hist.size() > 500) hist.erase(hist.begin());
    histPos = -1;
  }

  void moveCur(int delta) {
    Panel& p = cp();
    if (!p.isList()) return;
    p.cur += delta;
    p.clampCursor();
  }

  // ------------------------------------------------------------ running programs

  int spawn(const std::string& command) {
    const char* sh = getenv("SHELL");
    std::string shell = (sh && *sh) ? sh : "/bin/zsh";
    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGPIPE, SIG_DFL);
      signal(SIGWINCH, SIG_DFL);
      if (chdir(cp().realDir().c_str()) != 0) {}
      setenv("NCMAC_OTHER", op().path.c_str(), 1);
      execl(shell.c_str(), shell.c_str(), "-c", command.c_str(), (char*)nullptr);
      _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    for (;;) {
      pid_t r = waitpid(pid, &st, WUNTRACED);
      if (r < 0 && errno == EINTR) continue;
      if (r > 0 && WIFSTOPPED(st)) { kill(pid, SIGCONT); continue; }
      break;
    }
    return st;
  }

  void runShell(const std::string& command, bool echo, bool pause) {
    term::suspend();
    if (echo) term::write(prompt() + command + "\n");
    spawn(command);
    if (pause) {
      term::write("\n\x1b[7m Press any key to return to ncmac \x1b[0m");
      term::setRaw(true);
      int k;
      do k = term::readKey(-1); while (k == K_RESIZE || k == K_NONE || k == K_TIMEOUT);
      term::write("\r\n");
    }
    term::resume();
    reloadAll();
  }

  void openWithMac(const std::string& path) {
    pid_t pid = fork();
    if (pid == 0) {
      int fd = open("/dev/null", O_RDWR);
      if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
      execl("/usr/bin/open", "open", path.c_str(), (char*)nullptr);
      _exit(127);
    }
    if (pid > 0) {
      int st;
      waitpid(pid, &st, 0);
      if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
        ui::message("Open", {"No application is associated with", baseName(path)}, {"OK"});
    }
  }

  bool tryCd(const std::string& c) {
    std::string t = trim(c);
    std::string arg;
    if (t == "cd") arg = "";
    else if (t.rfind("cd ", 0) == 0 || t.rfind("cd\t", 0) == 0) arg = trim(t.substr(3));
    else if (t.size() > 2 && t.compare(0, 2, "cd") == 0 && (t[2] == '.' || t[2] == '/' || t[2] == '~' || t[2] == '\\'))
      arg = t.substr(2);  // DOS style: cd..  cd\  cd/
    else return false;
    if (arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') || (arg.front() == '\'' && arg.back() == '\'')))
      arg = arg.substr(1, arg.size() - 2);
    else {
      std::string u;
      for (size_t i = 0; i < arg.size(); i++) {
        if (arg[i] == '\\' && i + 1 < arg.size()) u += arg[++i];
        else u += arg[i];
      }
      arg = u;
    }
    if (arg == "\\") arg = "/";
    std::string target;
    if (arg.empty()) target = homeDir();
    else if (arg == "-") target = prevDir.empty() ? cp().path : prevDir;
    else target = resolvePath(cp().path, arg);
    if (!cp().visible) cp().visible = true;
    if (cp().view == View::Info) cp().view = View::Full;
    goDir(act, target);
    return true;
  }

  void execCommand(const std::string& c0) {
    std::string c = trim(c0);
    cmd.clear();
    if (c.empty()) return;
    addHistory(c);
    if (tryCd(c)) return;
    runShell(c, true, cfg.pause);
  }

  // ------------------------------------------------------------ user screen (Ctrl-O)

  void userScreen() {
    term::altScreen(false);  // stay in raw mode, show the normal screen
    std::u32string line;
    size_t pos = 0;
    int hp = -1;
    auto redraw = [&] {
      std::string out = "\r\x1b[K" + prompt() + fromU32(line);
      int back = u8width(fromU32(line.substr(pos)));
      if (back > 0) out += "\x1b[" + std::to_string(back) + "D";
      term::write(out);
    };
    redraw();
    for (;;) {
      int k = term::readKey(-1);
      if (k == CTRL('O')) break;
      if (k == K_RESIZE || k == K_NONE || k == K_TIMEOUT) continue;
      if (k == K_ENTER) {
        std::string c = trim(fromU32(line));
        term::write("\r\n");
        line.clear();
        pos = 0;
        hp = -1;
        if (!c.empty()) {
          addHistory(c);
          std::string before = cp().path;
          if (!tryCd(c)) {
            term::setRaw(false);
            spawn(c);
            term::setRaw(true);
          }
        }
        redraw();
        continue;
      }
      if (k == K_UP || k == CTRL('E')) {
        if (!hist.empty()) {
          hp = hp < 0 ? (int)hist.size() - 1 : std::max(0, hp - 1);
          line = toU32(hist[hp]);
          pos = line.size();
        }
      } else if (k == K_DOWN || k == CTRL('X')) {
        if (hp >= 0 && hp + 1 < (int)hist.size()) { hp++; line = toU32(hist[hp]); }
        else { hp = -1; line.clear(); }
        pos = line.size();
      } else if (k == K_ESC || k == CTRL('C')) {
        line.clear();
        pos = 0;
      } else if (isPrintable(k)) {
        line.insert(pos++, 1, (char32_t)k);
      } else if (k == K_BS) {
        if (pos > 0) line.erase(--pos, 1);
      } else if (k == K_DEL) {
        if (pos < line.size()) line.erase(pos, 1);
      } else if (k == K_LEFT) {
        if (pos > 0) pos--;
      } else if (k == K_RIGHT) {
        if (pos < line.size()) pos++;
      } else if (k == K_HOME || k == CTRL('A')) {
        pos = 0;
      } else if (k == K_END) {
        pos = line.size();
      }
      redraw();
    }
    term::write("\r\x1b[K");
    term::altScreen(true);
    reloadAll();
  }

  // ------------------------------------------------------------ panel actions

  void enter(bool forceInto = false) {
    Panel& p = cp();
    const Entry* e = p.current();
    if (!e) return;
    if (p.inArchive()) { enterInArchive(*e); return; }
    if (e->name == "..") { goDir(act, parentPath(p.path)); return; }
    std::string full = joinPath(p.path, e->name);
    if (e->dir) {
      std::string ext = toLower(e->name.size() > 4 ? e->name.substr(e->name.size() - 4) : "");
      if (!forceInto && ext == ".app") { openWithMac(full); return; }
      goDir(act, full);
      return;
    }
    if (forceInto) {
      if (!tryOpenArchive(full))
        ui::message("Archive", {"Not an archive, or a format that cannot be read:", e->name}, {"OK"});
      return;
    }
    if (arcLooksLike(e->name) && tryOpenArchive(full)) return;
    std::string n = e->name;
    auto dot = n.rfind('.');
    std::string ext = dot == std::string::npos ? "" : toLower(n.substr(dot + 1));
    bool script = dot == std::string::npos || dot == 0 || ext == "sh" || ext == "command" ||
                  ext == "py" || ext == "pl" || ext == "rb" || ext == "zsh" || ext == "bash" || ext == "tool";
    if (e->exec && script) {
      execCommand("./" + shellQuote(n));
      return;
    }
    openWithMac(full);
  }

  // ------------------------------------------------------------ archives

  std::string tmpRoot;
  int tmpCounter = 0;

  std::string tempDir() {
    if (tmpRoot.empty()) {
      const char* t = getenv("TMPDIR");
      tmpRoot = normPath((t && *t) ? t : "/tmp") + "/ncmac-" + std::to_string(getpid());
    }
    std::string d = tmpRoot + "/" + std::to_string(++tmpCounter);
    mkdirP(d);
    return d;
  }

  bool tryOpenArchive(const std::string& full) {
    std::string old = cp().path, err;
    ui::progress("Archive", {"Reading " + displayPath(full)}, -1, -1);
    if (!cp().openArchive(full, &err)) return false;
    prevDir = old;
    return true;
  }

  // extract one entry of the current archive panel to a temp dir; returns its path or ""
  std::string extractToTemp(const std::string& name) {
    Panel& p = cp();
    std::string d = tempDir();
    if (!fo_arcExtract(p.arcFile, p.arcDir, {name}, d, &p.arcIndex)) return "";
    std::string f = joinPath(d, name);
    return existsL(f) ? f : "";
  }

  void enterInArchive(const Entry& e) {
    Panel& p = cp();
    if (e.name == "..") {
      if (p.arcDir.empty()) goDir(act, parentPath(p.arcFile));
      else p.archiveCd(p.arcDir.find('/') == std::string::npos ? "" : parentPath("/" + p.arcDir).substr(1),
                       baseName(p.arcDir));
      return;
    }
    if (e.dir) {
      p.archiveCd(p.arcDir.empty() ? e.name : p.arcDir + "/" + e.name);
      return;
    }
    std::string f = extractToTemp(e.name);
    if (!f.empty()) openWithMac(f);
  }

  bool readOnly() {
    if (!cp().inArchive()) return false;
    ui::message("Archive", {"Archives are read-only.", "Use F5 to copy files out of the archive."}, {"OK"});
    return true;
  }

  void insertName(bool fullPath) {
    const Entry* e = cp().current();
    if (!e) return;
    std::string n = fullPath ? joinPath(cp().path, e->name) : e->name;
    if (e->name == ".." && !fullPath) n = "..";
    std::string s = shellQuote(n) + " ";
    for (char32_t c : toU32(s)) cmd.key((int)c);
  }

  void toggleMark(int idx, bool advance) {
    Panel& p = cp();
    if (!p.isList() || idx < 0 || idx >= (int)p.e.size()) return;
    if (p.e[idx].name != "..") p.e[idx].marked = !p.e[idx].marked;
    if (advance) moveCur(1);
  }

  void selectGroup(bool sel) {
    Panel& p = cp();
    if (!p.isList()) return;
    std::string mask = "*";
    if (!ui::input(sel ? "Select" : "Unselect", sel ? "Select files matching:" : "Unselect files matching:", mask))
      return;
    auto masks = splitMasks(mask);
    for (auto& e : p.e)
      if (!e.dir && matchMasks(masks, e.name)) e.marked = sel;
  }

  void invertSel() {
    for (auto& e : cp().e)
      if (!e.dir) e.marked = !e.marked;
  }

  void view() {
    const Entry* e = cp().current();
    if (!e) return;
    if (e->dir) { dirSize(); return; }
    if (cp().inArchive()) {
      std::string f = extractToTemp(e->name);
      if (!f.empty()) runViewer(f);
      return;
    }
    runViewer(joinPath(cp().path, e->name));
  }

  void dirSize() {
    auto names = cp().selection();
    if (names.empty()) return;
    unsigned long long bytes = 0, files = 0, dirs = 0;
    ui::progress("Directory size", {"Counting..."}, -1, -1);
    if (cp().inArchive()) {
      Panel& p = cp();
      std::string pre = p.arcDir.empty() ? "" : p.arcDir + "/";
      for (auto& n : names)
        for (auto& a : p.arcIndex)
          if (a.path == pre + n || a.path.rfind(pre + n + "/", 0) == 0) {
            if (a.dir) dirs++;
            else { files++; bytes += a.size; }
          }
    } else {
      for (auto& n : names) fo_dirSize(joinPath(cp().path, n), bytes, files, dirs);
    }
    std::string what = names.size() == 1 ? names[0] : std::to_string(names.size()) + " items";
    ui::message("Size", {what, "", fmtThousands(bytes) + " bytes", std::to_string(files) + " files, " +
                                                                     std::to_string(dirs) + " directories"},
                {"OK"});
  }

  void edit(const std::string& full) {
    if (cfg.extEditor) {
      const char* ed = getenv("VISUAL");
      if (!ed || !*ed) ed = getenv("EDITOR");
      std::string e = (ed && *ed) ? ed : "vi";
      runShell(e + " " + shellQuote(full), false, false);
    } else {
      runEditor(full);
      reloadAll();
    }
  }

  void editCurrent() {
    const Entry* e = cp().current();
    if (!e || e->dir) return;
    if (readOnly()) return;
    edit(joinPath(cp().path, e->name));
  }

  void editNew() {
    if (readOnly()) return;
    std::string name;
    if (!ui::input("Edit", "Edit file:", name) || trim(name).empty()) return;
    std::string full = resolvePath(cp().path, name);
    edit(full);
    cp().load();
    if (parentPath(full) == cp().path) cp().focus(baseName(full));
  }

  void copyMove(bool move, bool renameOnly = false) {
    Panel& p = cp();
    if (!p.isList()) return;
    if (move && readOnly()) return;
    auto names = p.selection();
    if (names.empty()) return;
    std::string what = names.size() == 1 ? "\"" + names[0] + "\"" : std::to_string(names.size()) + " files";
    std::string def;
    if (renameOnly) {
      if (names.size() != 1) names.resize(1), names[0] = p.current() ? p.current()->name : names[0];
      if (!p.current() || p.current()->name == "..") return;
      names = {p.current()->name};
      def = names[0];
      what = "\"" + names[0] + "\"";
    } else {
      def = displayPath(op().path);
      if (names.size() == 1 && op().path == p.path) def = names[0];
    }
    std::string dest = def;
    std::string title = move ? "Rename/Move" : "Copy";
    std::string prompt = (move ? "Rename or move " : "Copy ") + what + " to:";
    if (!ui::input(title, prompt, dest, &copyHist)) return;
    if (trim(dest).empty()) return;
    std::string full = resolvePath(p.realDir(), dest);
    for (auto& q : P)
      if (q.inArchive() && (full == q.arcFile || full.rfind(q.arcFile + "/", 0) == 0)) {
        ui::message("Archive", {"Cannot copy into an archive:", "archives are read-only."}, {"OK"});
        return;
      }
    if (!dest.empty() && dest.back() == '/' && !isDir(full)) mkdirP(full);
    if (p.inArchive()) fo_arcExtract(p.arcFile, p.arcDir, names, full, &p.arcIndex);
    else fo_copyMove(p.path, names, full, move);
    for (auto& e : p.e) e.marked = false;
    reloadAll();
    if (move && names.size() == 1 && parentPath(full) == p.path && !isDir(full)) p.focus(baseName(full));
  }

  void makeDir() {
    if (readOnly()) return;
    std::string name;
    if (!ui::input("Make directory", "Create the directory:", name, &mkdirHist)) return;
    name = trim(name);
    if (name.empty()) return;
    std::string full = resolvePath(cp().path, name);
    if (mkdirP(full) != 0) {
      ui::error("Cannot create " + displayPath(full) + ": " + strerror(errno));
      return;
    }
    cp().load();
    if (full.compare(0, cp().path.size(), cp().path) == 0) {
      std::string rest = full.substr(cp().path == "/" ? 1 : cp().path.size() + 1);
      cp().focus(rest.substr(0, rest.find('/')));
    }
    if (op().path == cp().path) op().load();
  }

  bool inTrash(const std::string& path) const {
    std::string t = homeDir() + "/.Trash";
    if (path == t || path.rfind(t + "/", 0) == 0) return true;
    return path.find("/.Trashes/") != std::string::npos ||
           (path.size() >= 9 && path.compare(path.size() - 9, 9, "/.Trashes") == 0);
  }

  void deleteFiles(bool permanent = false) {
    Panel& p = cp();
    if (!p.isList() || readOnly()) return;
    auto names = p.selection();
    if (names.empty()) return;
    std::string what = names.size() == 1 ? "\"" + names[0] + "\"" : std::to_string(names.size()) + " files/directories";
    if (cfg.trash && !permanent && !inTrash(p.path)) {
      int r = ui::message("Delete", {"Move " + what, "to the Trash?"}, {"Trash", "Cancel"});
      if (r != 0) return;
      fo_trash(p.path, names);
    } else {
      int r = ui::message(cfg.trash ? "Delete permanently" : "Delete",
                          {"Do you wish to delete", what + "?", cfg.trash ? "This cannot be undone." : ""},
                          {"Delete", "Cancel"}, true);
      if (r != 0) return;
      fo_delete(p.path, names);
    }
    for (auto& e : p.e) e.marked = false;
    reloadAll();
  }

  // ------------------------------------------------------------ dialogs

  void driveMenu(int i) {
    std::vector<std::string> paths = {"/", homeDir()};
    for (const char* sub : {"Desktop", "Documents", "Downloads"}) {
      std::string d = homeDir() + "/" + sub;
      if (isDir(d)) paths.push_back(d);
    }
    paths.push_back("/Applications");
    if (DIR* d = opendir("/Volumes")) {
      while (dirent* de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        std::string v = std::string("/Volumes/") + de->d_name;
        char rp[PATH_MAX];
        if (realpath(v.c_str(), rp) && std::string(rp) == "/") continue;
        paths.push_back(v);
      }
      closedir(d);
    }
    std::vector<std::string> labels;
    int sel = 0;
    for (size_t j = 0; j < paths.size(); j++) {
      char letter = (char)(j < 26 ? 'A' + j : '0' + (j - 26) % 10);
      labels.push_back(std::string(1, letter) + "  " + displayPath(paths[j]));
      if (P[i].path == paths[j] || (P[i].path.rfind(paths[j] + "/", 0) == 0 && paths[j] != "/")) sel = (int)j;
    }
    int r = ui::list(i == 0 ? "Left - Drive" : "Right - Drive", labels, sel, true);
    if (r < 0) return;
    P[i].visible = true;
    if (P[i].view == View::Info) P[i].view = P[i].prevView == View::Info ? View::Full : P[i].prevView;
    act = i;
    goDir(i, paths[r]);
  }

  void historyDialog() {
    if (hist.empty()) { ui::message("History", {"The command history is empty."}, {"OK"}); return; }
    std::vector<std::string> items(hist.rbegin(), hist.rend());
    int r = ui::list("Command history", items, 0);
    if (r >= 0) cmd.set(items[r]);
  }

  void findFile() {
    std::string mask = findHist.empty() ? "*" : findHist.back();
    if (!ui::input("Find file", "Find files matching (e.g. *.txt;*.md):", mask, &findHist)) return;
    auto masks = splitMasks(mask);
    std::string root = cp().realDir();
    std::vector<std::string> results;
    std::vector<std::string> stack{root};
    auto last = std::chrono::steady_clock::now();
    bool stopped = false;
    while (!stack.empty() && !stopped) {
      std::string dir = stack.back();
      stack.pop_back();
      auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::milliseconds(100)) {
        last = now;
        ui::progress("Find file", {"Searching " + displayPath(dir), std::to_string(results.size()) + " found  (Esc to stop)"}, -1, -1);
        int k = term::readKey(0);
        if (k == K_ESC) stopped = true;
      }
      DIR* d = opendir(dir.c_str());
      if (!d) continue;
      std::vector<std::string> subdirs;
      while (dirent* de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string full = joinPath(dir, de->d_name);
        bool isd = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
          struct stat st;
          isd = lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (matchMasks(masks, de->d_name)) results.push_back(full);
        if (isd) subdirs.push_back(full);
      }
      closedir(d);
      std::sort(subdirs.rbegin(), subdirs.rend());
      for (auto& s : subdirs) stack.push_back(s);
      if (results.size() >= 10000) break;
    }
    if (results.empty()) {
      ui::message("Find file", {"No files found."}, {"OK"});
      return;
    }
    std::vector<std::string> items;
    for (auto& r : results) {
      std::string rel = r.substr(root == "/" ? 1 : root.size() + 1);
      items.push_back(rel);
    }
    int r = ui::list("Find file - " + std::to_string(results.size()) + " found (Enter = go to file)", items, 0);
    if (r < 0) return;
    std::string target = results[r];
    goDir(act, parentPath(target));
    cp().focus(baseName(target));
  }

  // ------------------------------------------------------------ directory tree (Alt-F10)

  void treeDialog() {
    std::map<std::string, std::vector<std::string>> cache;
    const std::string here = cp().path;
    auto onPath = [&](const std::string& x) { return here == x || here.rfind(x + "/", 0) == 0; };
    auto kids = [&](const std::string& p) -> const std::vector<std::string>& {
      auto it = cache.find(p);
      if (it != cache.end()) return it->second;
      std::vector<std::string> v;
      if (DIR* d = opendir(p.c_str())) {
        while (dirent* de = readdir(d)) {
          std::string n = de->d_name;
          if (n == "." || n == "..") continue;
          std::string full = joinPath(p, n);
          bool keep = onPath(full);  // always show the way to the current directory
          if (!keep && !cfg.showHidden && n[0] == '.') continue;
          if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN) continue;  // skips symlinks: no cycles
          struct stat st;
          if (lstat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
          if (!keep && !cfg.showHidden && (st.st_flags & UF_HIDDEN)) continue;
          v.push_back(n);
        }
        closedir(d);
      }
      std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        int r = strcasecmp(a.c_str(), b.c_str());
        return r ? r < 0 : a < b;
      });
      return cache[p] = v;
    };

    struct Line {
      std::string path, name, prefix;
    };
    std::vector<Line> lines;
    std::set<std::string> expanded;
    for (std::string a = cp().path;; a = parentPath(a)) {
      expanded.insert(a);
      if (a == "/") break;
    }
    std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& p,
                                                                          const std::string& prefix) {
      const auto& k = kids(p);
      for (size_t i = 0; i < k.size(); i++) {
        bool last = i + 1 == k.size();
        std::string path = joinPath(p, k[i]);
        lines.push_back({path, k[i], prefix + (last ? "└──" : "├──")});
        if (expanded.count(path)) walk(path, prefix + (last ? "   " : "│  "));
      }
    };
    int sel = 0, top = 0;
    auto rebuild = [&](const std::string& focusPath) {
      lines.clear();
      lines.push_back({"/", "/", ""});
      walk("/", "");
      sel = 0;
      for (size_t i = 0; i < lines.size(); i++)
        if (lines[i].path == focusPath) { sel = (int)i; break; }
    };
    rebuild(cp().path);

    std::string search;
    auto lastKey = std::chrono::steady_clock::now();
    for (;;) {
      ui::backdrop();
      Attr fr = mk(C_LCYAN, C_BLUE), hl = mk(C_BLACK, C_CYAN), dim = mk(C_CYAN, C_BLUE);
      int x = 2, y = 1, w = std::max(20, scr.w - 4), h = std::max(8, scr.h - 4);
      int rows = h - 4;
      int n = (int)lines.size();
      sel = std::max(0, std::min(sel, n - 1));
      if (sel < top) top = sel;
      if (sel >= top + rows) top = sel - rows + 1;
      scr.rect(x, y, w, h, fr);
      scr.box(x, y, w, h, fr, true);
      for (int r = y + 1; r <= y + h; r++) { scr.shade(x + w, r); scr.shade(x + w + 1, r); }
      for (int c = x + 2; c < x + w; c++) scr.shade(c, y + h);
      std::string t = " Directory tree ";
      scr.put(x + (w - u8width(t)) / 2, y, t, fr);
      for (int r = 0; r < rows && top + r < n; r++) {
        const Line& L = lines[top + r];
        int ry = y + 1 + r;
        int px = x + 2 + scr.put(x + 2, ry, L.prefix, dim);
        const std::string& name = L.name;
        int room = x + w - 2 - px;
        scr.put(px, ry, u8fit(" " + name + " ", std::min(room, u8width(name) + 2)),
                top + r == sel ? hl : (L.path == cp().path ? mk(C_YELLOW, C_BLUE) : mk(C_WHITE, C_BLUE)));
      }
      int sy = y + h - 3;
      scr.set(x, sy, "╟", fr);
      scr.fill(x + 1, sy, w - 2, fr, "─");
      scr.set(x + w - 1, sy, "╢", fr);
      std::string info = search.empty() ? displayPath(lines[sel].path) : "Search: " + search;
      scr.put(x + 2, sy + 1, u8fit(u8truncLeft(info, w - 4), w - 4), search.empty() ? fr : mk(C_YELLOW, C_BLUE));
      scr.hideCursor();
      scr.flush();

      int k = term::readKey(-1);
      auto now = std::chrono::steady_clock::now();
      bool fresh = now - lastKey > std::chrono::milliseconds(1500);
      lastKey = now;
      const Line cur = lines[sel];
      switch (k) {
        case K_UP: sel--; search.clear(); break;
        case K_DOWN: sel++; search.clear(); break;
        case K_PGUP: sel -= rows - 1; search.clear(); break;
        case K_PGDN: sel += rows - 1; search.clear(); break;
        case K_HOME: sel = 0; search.clear(); break;
        case K_END: sel = n - 1; search.clear(); break;
        case K_RIGHT: case '+':
          search.clear();
          if (!expanded.count(cur.path) || cur.path == "/") {
            expanded.insert(cur.path);
            rebuild(cur.path);
          } else if (sel + 1 < n && parentPath(lines[sel + 1].path) == cur.path) {
            sel++;
          }
          break;
        case K_LEFT: case '-':
          search.clear();
          if (expanded.count(cur.path) && cur.path != "/") {
            for (auto it = expanded.begin(); it != expanded.end();) {  // collapse whole subtree
              if (*it == cur.path || it->rfind(cur.path + "/", 0) == 0) it = expanded.erase(it);
              else ++it;
            }
            rebuild(cur.path);
          } else if (cur.path != "/") {
            rebuild(parentPath(cur.path));
          }
          break;
        case '*':  // expand everything below (one level deeper each time)
          for (auto& L : std::vector<Line>(lines))
            if (L.path == cur.path || L.path.rfind(cur.path == "/" ? "/" : cur.path + "/", 0) == 0)
              expanded.insert(L.path);
          rebuild(cur.path);
          break;
        case CTRL('R'): case KF(2):
          cache.clear();
          rebuild(cur.path);
          break;
        case K_ENTER:
          P[act].visible = true;
          if (P[act].view == View::Info) P[act].view = View::Full;
          goDir(act, cur.path);
          return;
        case K_ESC: case KF(10): case KF(10) | KM_ALT:
          if (!search.empty()) { search.clear(); break; }
          return;
        case K_BS:
          if (!search.empty()) search.pop_back();
          break;
        case K_MOUSE:
          if (g_mouse.wheelUp) sel -= 3;
          else if (g_mouse.wheelDown) sel += 3;
          else if (g_mouse.press) {
            int r = g_mouse.y - (y + 1);
            if (r >= 0 && r < rows && top + r < n) {
              if (top + r == sel) {  // second click on the same line: go there
                goDir(act, lines[sel].path);
                return;
              }
              sel = top + r;
            } else if (g_mouse.x < x || g_mouse.x >= x + w || g_mouse.y < y || g_mouse.y >= y + h) {
              return;
            }
          }
          break;
        default:
          if (isPrintable(keyBase(k)) && (k & ~KM_ALT) < K_SPECIAL) {
            uint32_t c = (uint32_t)keyBase(k);
            if (fresh) search.clear();
            std::string cand = search + u8encode(c);
            std::string lc = toLower(cand);
            int start = search.empty() ? sel + 1 : sel;
            for (int d = 0; d < n; d++) {
              int j = (start + d) % n;
              if (toLower(lines[j].name).rfind(lc, 0) == 0) {
                sel = j;
                search = cand;
                break;
              }
            }
          }
      }
    }
  }

  void compareDirs() {
    Panel& a = cp();
    Panel& b = op();
    if (!a.isList() || !b.isList()) return;
    auto mark = [](Panel& x, Panel& y) {
      for (auto& e : x.e) {
        if (e.dir) { e.marked = false; continue; }
        auto it = std::find_if(y.e.begin(), y.e.end(), [&](const Entry& o) { return o.name == e.name && !o.dir; });
        e.marked = (it == y.e.end()) || e.mtime > it->mtime || (e.mtime == it->mtime && e.size != it->size);
      }
    };
    mark(a, b);
    mark(b, a);
  }

  void toggleRows() {
    static const int cyc[] = {25, 43, 50};
    int next = 25;
    for (int i = 0; i < 3; i++)
      if (scr.h == cyc[i]) next = (i == 2) ? origH : cyc[i + 1];
    if (next == scr.h) next = 25;
    term::requestSize(scr.w, next);
    resizedTerm = true;
  }

  // ------------------------------------------------------------ user menu (F2)

  struct UItem {
    std::string key, label;
    std::vector<std::string> cmds;
  };

  std::string menuFile() const { return configDir() + "/menu"; }

  void writeDefaultMenu(const std::string& f) {
    mkdirP(parentPath(f));
    std::ofstream o(f);
    o << "# ncmac user menu (F2)\n"
         "# A line '<hotkey>: <description>' starts an entry; the indented lines below it\n"
         "# are the shell commands. Substitutions:\n"
         "#   %f  current file      %s  selected files (or current)\n"
         "#   %d  current directory %o  other panel's directory    !.!  same as %f\n"
         "# A file named .ncmenu in a directory overrides this menu there.\n\n"
         "d: Disk usage of this directory\n"
         "    du -sh * | sort -h\n"
         "g: Git status\n"
         "    git status\n"
         "l: Git log (last 20)\n"
         "    git log --oneline -20\n"
         "z: Zip selected files into archive.zip\n"
         "    zip -r archive.zip %s\n"
         "u: Unpack current archive\n"
         "    tar -xvf %f\n"
         "q: Quick Look current file\n"
         "    qlmanage -p %f >/dev/null 2>&1\n"
         "r: Reveal current file in Finder\n"
         "    open -R %f\n"
         "c: Copy full path of current file to clipboard\n"
         "    printf '%%s' %d/%f | pbcopy\n";
  }

  std::vector<UItem> parseMenu(const std::string& f) {
    std::vector<UItem> items;
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      if (line[0] == ' ' || line[0] == '\t') {
        if (!items.empty() && !trim(line).empty()) items.back().cmds.push_back(trim(line));
        continue;
      }
      auto c = line.find(':');
      if (c == std::string::npos) continue;
      UItem it;
      it.key = trim(line.substr(0, c));
      it.label = trim(line.substr(c + 1));
      items.push_back(it);
    }
    return items;
  }

  std::string substitute(const std::string& s) {
    const Entry* e = cp().current();
    std::string f = e ? shellQuote(e->name) : "";
    std::string sel;
    for (auto& n : cp().selection()) sel += (sel.empty() ? "" : " ") + shellQuote(n);
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
      if (s.compare(i, 3, "!.!") == 0) { r += f; i += 2; continue; }
      if (s[i] == '%' && i + 1 < s.size()) {
        char c = s[i + 1];
        if (c == 'f') { r += f; i++; continue; }
        if (c == 's') { r += sel; i++; continue; }
        if (c == 'd') { r += shellQuote(cp().realDir()); i++; continue; }
        if (c == 'o') { r += shellQuote(op().path); i++; continue; }
        if (c == '%') { r += '%'; i++; continue; }
      }
      r += s[i];
    }
    return r;
  }

  void userMenu() {
    std::string local = joinPath(cp().path, ".ncmenu");
    std::string f = existsL(local) ? local : menuFile();
    if (!existsL(f)) {
      int r = ui::message("User menu", {"There is no user menu yet.", "Create " + displayPath(f) + " with examples?"},
                          {"Yes", "No"});
      if (r == 0) {
        writeDefaultMenu(f);
        runEditor(f);
      }
      return;
    }
    auto items = parseMenu(f);
    if (items.empty()) {
      ui::message("User menu", {"The user menu is empty:", displayPath(f)}, {"OK"});
      return;
    }
    std::vector<std::string> labels;
    for (auto& it : items) labels.push_back(u8fit(it.key, 3) + it.label);
    int r = ui::list("User Menu", labels, 0, true);
    if (r < 0 || items[r].cmds.empty()) return;
    std::string script;
    for (auto& c : items[r].cmds) script += substitute(c) + "\n";
    runShell(script, false, true);
  }

  // ------------------------------------------------------------ help

  void help() {
    ui::text("ncmac - Help",
             {"Panels",
              "  Tab                  switch panel",
              "  Arrows PgUp PgDn     move;  Home/End first/last",
              "  Enter                enter directory or archive / run script / open with macOS",
              "                       (zip, tar.gz/bz2/xz/zst, 7z, rar, iso, ... are read-only;",
              "                        F3 views and F5 copies files out of an archive)",
              "  Ctrl-PgUp            parent directory     Ctrl-PgDn  enter (also .app)",
              "  Ctrl-\\               root directory",
              "  Insert  or Ctrl-T    select file          + - *  select/unselect/invert",
              "  Alt-letter, Ctrl-S   quick search (Option key as Meta)",
              "  Ctrl-O               show user screen / command output",
              "  Ctrl-P  Ctrl-U       other panel on/off, swap panels",
              "  Ctrl-L               info panel           Ctrl-R  re-read",
              "  Ctrl-F1 / Ctrl-F2    left / right panel on/off",
              "  Ctrl-F3..F7          sort name/ext/time/size/unsorted",
              "  Alt-F1 / Alt-F2      choose 'drive' (volume / favourite)",
              "",
              "Function keys",
              "  F1 help   F2 user menu   F3 view   F4 edit   F5 copy",
              "  F6 rename/move   F7 mkdir   F8 delete (to Trash)   F9 menu   F10 quit",
              "  Shift-F8 delete permanently (in the Trash itself F8 is permanent too)",
              "  Shift-F4 edit new file    Shift-F6 rename in place",
              "  Alt-F7 find file    Alt-F8 history    Alt-F9 25/43/50 lines",
              "  Alt-F10 directory tree (Right/+ open, Left/- close, type to search)",
              "  Shift-F9 save setup    Alt-. show/hide hidden files (also F9 Options)",
              "  Without F-keys (iPad): Ctrl-F then 1..9,0 = F1..F10",
              "    Ctrl-F s 4 = Shift-F4, Ctrl-F a 7 = Alt-F7, Ctrl-F c 3 = Ctrl-F3",
              "  Also: Esc 1 .. Esc 0 (quickly), or tap the key bar",
              "",
              "Command line",
              "  Type a command, Enter runs it in $SHELL in the current directory.",
              "  cd, cd.., cd -, cd ~ are handled internally.",
              "  Ctrl-E / Ctrl-X     previous / next command",
              "  Ctrl-J              put current file name on the command line",
              "  Alt-Enter           put full path on the command line",
              "  Esc                 clear the command line",
              "",
              "Mouse: click selects, double-click enters, right-click marks,",
              "       wheel scrolls, click on the key bar presses that key."});
  }

  // ------------------------------------------------------------ setup

  std::string cfgFile() const { return configDir() + "/ncmac.conf"; }

  static const char* viewName(View v) { return v == View::Brief ? "brief" : v == View::Info ? "info" : "full"; }
  static const char* sortName(Sort s) {
    switch (s) {
      case Sort::Ext: return "ext";
      case Sort::Time: return "time";
      case Sort::Size: return "size";
      case Sort::Unsorted: return "unsorted";
      default: return "name";
    }
  }

  void saveSetup() {
    mkdirP(configDir());
    std::ofstream o(cfgFile());
    o << "hidden=" << cfg.showHidden << "\npause=" << cfg.pause << "\nmouse=" << cfg.mouse
      << "\nclock=" << cfg.clock << "\nexteditor=" << cfg.extEditor << "\ntrash=" << cfg.trash << "\n";
    const char* side[2] = {"left", "right"};
    for (int i = 0; i < 2; i++) {
      View v = P[i].view == View::Info ? View::Full : P[i].view;
      o << side[i] << ".view=" << viewName(v) << "\n"
        << side[i] << ".sort=" << sortName(P[i].sort) << "\n"
        << side[i] << ".reverse=" << P[i].reverse << "\n"
        << side[i] << ".visible=" << P[i].visible << "\n";
    }
    ui::message("Setup", {"Setup saved to", displayPath(cfgFile())}, {"OK"});
  }

  void loadSetup() {
    std::ifstream in(cfgFile());
    std::string line;
    while (std::getline(in, line)) {
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
      bool b = v == "1" || v == "true";
      if (k == "hidden") cfg.showHidden = b;
      else if (k == "pause") cfg.pause = b;
      else if (k == "mouse") cfg.mouse = b;
      else if (k == "clock") cfg.clock = b;
      else if (k == "exteditor") cfg.extEditor = b;
      else if (k == "trash") cfg.trash = b;
      else {
        int i = k.rfind("left.", 0) == 0 ? 0 : k.rfind("right.", 0) == 0 ? 1 : -1;
        if (i < 0) continue;
        std::string sub = k.substr(k.find('.') + 1);
        if (sub == "view") P[i].view = v == "brief" ? View::Brief : View::Full;
        else if (sub == "sort")
          P[i].sort = v == "ext" ? Sort::Ext : v == "time" ? Sort::Time : v == "size" ? Sort::Size
                    : v == "unsorted" ? Sort::Unsorted : Sort::Name;
        else if (sub == "reverse") P[i].reverse = b;
        else if (sub == "visible") P[i].visible = b;
      }
    }
    if (!P[0].visible && !P[1].visible) P[0].visible = P[1].visible = true;
    if (!P[act].visible) act = 1 - act;
  }

  void loadHistory() {
    std::ifstream in(configDir() + "/history");
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) hist.push_back(line);
  }

  void saveHistory() {
    mkdirP(configDir());
    std::ofstream o(configDir() + "/history");
    size_t start = hist.size() > 500 ? hist.size() - 500 : 0;
    for (size_t i = start; i < hist.size(); i++)
      if (hist[i].find('\n') == std::string::npos) o << hist[i] << "\n";
  }

  // ------------------------------------------------------------ pull-down menu (F9)

  std::vector<Menu> buildMenus() {
    auto panelMenu = [this](int i) {
      Menu m;
      m.title = i == 0 ? "Left" : "Right";
      Panel* pp = &P[i];
      auto pid = [i](int o) { return A_PANEL + i * 100 + o; };
      m.items = {
          {"Brief", "", pid(P_BRIEF), [pp] { return pp->visible && pp->view == View::Brief; }},
          {"Full", "", pid(P_FULL), [pp] { return pp->visible && pp->view == View::Full; }},
          {"Info", "Ctrl-L", pid(P_INFO), [pp] { return pp->visible && pp->view == View::Info; }},
          {"On/Off", i == 0 ? "Ctrl-F1" : "Ctrl-F2", pid(P_TOGGLE), [pp] { return pp->visible; }},
          {"-", "", 0, nullptr},
          {"Name", "Ctrl-F3", pid(P_SNAME), [pp] { return pp->sort == Sort::Name; }},
          {"Extension", "Ctrl-F4", pid(P_SEXT), [pp] { return pp->sort == Sort::Ext; }},
          {"Time", "Ctrl-F5", pid(P_STIME), [pp] { return pp->sort == Sort::Time; }},
          {"Size", "Ctrl-F6", pid(P_SSIZE), [pp] { return pp->sort == Sort::Size; }},
          {"Unsorted", "Ctrl-F7", pid(P_SUNS), [pp] { return pp->sort == Sort::Unsorted; }},
          {"Reverse order", "", pid(P_REV), [pp] { return pp->reverse; }},
          {"-", "", 0, nullptr},
          {"Re-read", "Ctrl-R", pid(P_REREAD), nullptr},
          {"Drive...", i == 0 ? "Alt-F1" : "Alt-F2", pid(P_DRIVE), nullptr},
      };
      return m;
    };
    Menu files{"Files", {
        {"Help", "F1", A_HELP, nullptr},
        {"User menu", "F2", A_USERMENU, nullptr},
        {"View", "F3", A_VIEW, nullptr},
        {"Edit", "F4", A_EDIT, nullptr},
        {"New file", "Shift-F4", A_NEWFILE, nullptr},
        {"Copy", "F5", A_COPY, nullptr},
        {"Rename or move", "F6", A_MOVE, nullptr},
        {"Rename in place", "Shift-F6", A_RENAME, nullptr},
        {"Make directory", "F7", A_MKDIR, nullptr},
        {"Delete", "F8", A_DELETE, nullptr},
        {"Delete permanently", "Shift-F8", A_DELPERM, nullptr},
        {"-", "", 0, nullptr},
        {"Select group", "+", A_SELECT, nullptr},
        {"Unselect group", "-", A_UNSELECT, nullptr},
        {"Invert selection", "*", A_INVERT, nullptr},
        {"-", "", 0, nullptr},
        {"Quit", "F10", A_QUIT, nullptr}}, 0};
    Menu cmds{"Commands", {
        {"Find file", "Alt-F7", A_FIND, nullptr},
        {"Directory tree", "Alt-F10", A_TREE, nullptr},
        {"History", "Alt-F8", A_HISTORY, nullptr},
        {"Directory size", "", A_DIRSIZE, nullptr},
        {"Compare directories", "", A_COMPARE, nullptr},
        {"-", "", 0, nullptr},
        {"Swap panels", "Ctrl-U", A_SWAP, nullptr},
        {"Panels on/off", "Ctrl-O", A_PANELS, nullptr},
        {"Open in Finder", "", A_FINDER, nullptr},
        {"-", "", 0, nullptr},
        {"Edit user menu", "", A_EDITMENU, nullptr}}, 0};
    Menu opts{"Options", {
        {"Show hidden files", "Alt-.", A_OPT_HIDDEN, [] { return cfg.showHidden; }},
        {"Pause after command", "", A_OPT_PAUSE, [] { return cfg.pause; }},
        {"Mouse", "", A_OPT_MOUSE, [] { return cfg.mouse; }},
        {"Clock", "", A_OPT_CLOCK, [] { return cfg.clock; }},
        {"External editor ($EDITOR)", "", A_OPT_EXTEDIT, [] { return cfg.extEditor; }},
        {"Delete to Trash (F8)", "", A_OPT_TRASH, [] { return cfg.trash; }},
        {"Screen lines 25/43/50", "Alt-F9", A_ROWS, nullptr},
        {"-", "", 0, nullptr},
        {"Save setup", "Shift-F9", A_SAVE, nullptr}}, 0};
    return {panelMenu(0), files, cmds, opts, panelMenu(1)};
  }

  int runMenu() {
    auto menus = buildMenus();
    int n = (int)menus.size();
    int m = act == 0 ? 0 : n - 1;
    bool open = true;
    auto isSep = [&](int mi, int j) { return menus[mi].items[j].label == "-"; };
    auto first = [&](int mi) {
      for (int j = 0; j < (int)menus[mi].items.size(); j++)
        if (!isSep(mi, j)) return j;
      return 0;
    };
    int it = first(m);
    for (;;) {
      drawAll();
      scr.hideCursor();
      Attr bar = mk(C_BLACK, C_CYAN), sel = mk(C_WHITE, C_BLACK);
      scr.fill(0, 0, scr.w, bar);
      int x = 2;
      for (int i = 0; i < n; i++) {
        menus[i].x = x;
        std::string t = "  " + menus[i].title + "  ";
        scr.put(x, 0, t, i == m ? sel : bar);
        x += u8width(t);
      }
      int bx = 0, by = 1, bw = 0, bh = 0;
      if (open) {
        auto& M = menus[m];
        int lw = 0, kw = 0;
        for (auto& i : M.items) { lw = std::max(lw, u8width(i.label)); kw = std::max(kw, u8width(i.key)); }
        bw = lw + kw + 8;
        bh = (int)M.items.size() + 2;
        bx = std::max(0, std::min(M.x, scr.w - bw - 2));
        scr.rect(bx, by, bw, bh, bar);
        scr.box(bx, by, bw, bh, bar, true);
        for (int r = by + 1; r <= by + bh; r++) { scr.shade(bx + bw, r); scr.shade(bx + bw + 1, r); }
        for (int c = bx + 2; c < bx + bw; c++) scr.shade(c, by + bh);
        for (int j = 0; j < (int)M.items.size(); j++) {
          auto& I = M.items[j];
          int ry = by + 1 + j;
          if (I.label == "-") {
            scr.set(bx, ry, "╟", bar);
            scr.fill(bx + 1, ry, bw - 2, bar, "─");
            scr.set(bx + bw - 1, ry, "╢", bar);
            continue;
          }
          Attr a = j == it ? sel : bar;
          std::string chk = (I.checked && I.checked()) ? "√" : " ";
          std::string s = " " + chk + " " + u8fit(I.label, lw) + "  " + u8rjust(I.key, kw) + " ";
          scr.put(bx + 1, ry, u8fit(s, bw - 2), a);
          if (j != it) scr.put(bx + 4, ry, I.label.substr(0, 1), mk(C_YELLOW, C_CYAN));
        }
      }
      scr.flush();
      int k = term::readKey(-1);
      auto& M = menus[m];
      int cnt = (int)M.items.size();
      switch (k) {
        case K_LEFT: m = (m + n - 1) % n; it = first(m); break;
        case K_RIGHT: m = (m + 1) % n; it = first(m); break;
        case K_DOWN:
          if (!open) { open = true; break; }
          do it = (it + 1) % cnt; while (isSep(m, it));
          break;
        case K_UP:
          if (!open) { open = true; break; }
          do it = (it + cnt - 1) % cnt; while (isSep(m, it));
          break;
        case K_HOME: it = first(m); break;
        case K_END: it = cnt - 1; break;
        case K_ENTER:
          if (!open) { open = true; break; }
          return M.items[it].action;
        case K_ESC: case KF(9): case KF(10): return 0;
        case K_MOUSE:
          if (!g_mouse.press) break;
          if (g_mouse.y == 0) {
            for (int i = n - 1; i >= 0; i--)
              if (g_mouse.x >= menus[i].x) { m = i; it = first(m); open = true; break; }
            break;
          }
          if (open && g_mouse.x > bx && g_mouse.x < bx + bw - 1 && g_mouse.y > by && g_mouse.y < by + bh - 1) {
            int j = g_mouse.y - by - 1;
            if (!isSep(m, j)) return M.items[j].action;
            break;
          }
          return 0;
        default:
          if (isPrintable(k) && k < 128) {
            int c = tolower(k);
            if (open) {
              for (int j = 0; j < cnt; j++)
                if (!isSep(m, j) && tolower(M.items[j].label[0]) == c) return M.items[j].action;
            } else {
              for (int i = 0; i < n; i++)
                if (tolower(menus[i].title[0]) == c) { m = i; it = first(m); open = true; break; }
            }
          }
      }
    }
  }

  void setView(int i, View v) {
    Panel& p = P[i];
    p.visible = true;
    if (v == View::Info) {
      if (p.view != View::Info) p.prevView = p.view;
      p.view = View::Info;
      if (act == i && P[1 - i].isList()) act = 1 - i;
    } else {
      p.view = v;
    }
    p.clampCursor();
  }

  void togglePanel(int i) {
    P[i].visible = !P[i].visible;
    if (!P[i].visible && act == i && P[1 - i].isList()) act = 1 - i;
    if (P[i].visible && !P[act].isList() && P[i].view != View::Info) act = i;
  }

  void setSort(int i, Sort s) {
    if (P[i].sort == s) P[i].reverse = !P[i].reverse;
    else { P[i].sort = s; P[i].reverse = false; }
    std::string name = P[i].current() ? P[i].current()->name : "";
    P[i].sortEntries();
    P[i].focus(name);
  }

  void doAction(int a) {
    if (a >= A_PANEL) {
      int i = (a - A_PANEL) / 100, o = (a - A_PANEL) % 100;
      switch (o) {
        case P_BRIEF: setView(i, View::Brief); break;
        case P_FULL: setView(i, View::Full); break;
        case P_INFO: setView(i, P[i].view == View::Info ? P[i].prevView : View::Info); break;
        case P_TOGGLE: togglePanel(i); break;
        case P_SNAME: setSort(i, Sort::Name); break;
        case P_SEXT: setSort(i, Sort::Ext); break;
        case P_STIME: setSort(i, Sort::Time); break;
        case P_SSIZE: setSort(i, Sort::Size); break;
        case P_SUNS: P[i].sort = Sort::Unsorted; P[i].load(); break;
        case P_REV: P[i].reverse = !P[i].reverse; P[i].sortEntries(); break;
        case P_REREAD: P[i].load(); break;
        case P_DRIVE: driveMenu(i); break;
      }
      return;
    }
    switch (a) {
      case A_HELP: help(); break;
      case A_USERMENU: userMenu(); break;
      case A_VIEW: view(); break;
      case A_EDIT: editCurrent(); break;
      case A_NEWFILE: editNew(); break;
      case A_COPY: copyMove(false); break;
      case A_MOVE: copyMove(true); break;
      case A_RENAME: copyMove(true, true); break;
      case A_MKDIR: makeDir(); break;
      case A_DELETE: deleteFiles(); break;
      case A_DELPERM: deleteFiles(true); break;
      case A_SELECT: selectGroup(true); break;
      case A_UNSELECT: selectGroup(false); break;
      case A_INVERT: invertSel(); break;
      case A_QUIT: quit(); break;
      case A_FIND: findFile(); break;
      case A_TREE: treeDialog(); break;
      case A_HISTORY: historyDialog(); break;
      case A_DIRSIZE: dirSize(); break;
      case A_COMPARE: compareDirs(); break;
      case A_SWAP: std::swap(P[0], P[1]); act = 1 - act; break;
      case A_PANELS: userScreen(); break;
      case A_FINDER: openWithMac(cp().realDir()); break;
      case A_EDITMENU:
        if (!existsL(menuFile())) writeDefaultMenu(menuFile());
        runEditor(menuFile());
        break;
      case A_ROWS: toggleRows(); break;
      case A_OPT_HIDDEN: cfg.showHidden = !cfg.showHidden; reloadAll(); break;
      case A_OPT_PAUSE: cfg.pause = !cfg.pause; break;
      case A_OPT_MOUSE: cfg.mouse = !cfg.mouse; term::setMouse(cfg.mouse); break;
      case A_OPT_CLOCK: cfg.clock = !cfg.clock; break;
      case A_OPT_EXTEDIT: cfg.extEditor = !cfg.extEditor; break;
      case A_OPT_TRASH: cfg.trash = !cfg.trash; break;
      case A_SAVE: saveSetup(); break;
    }
  }

  void quit() {
    int r = ui::message("ncmac", {"Do you want to quit ncmac?"}, {"Yes", "No"});
    if (r == 0) running = false;
  }

  // ------------------------------------------------------------ quick search

  bool qsFind(const std::string& t) {
    std::string lt = toLower(t);
    Panel& p = cp();
    for (size_t i = 0; i < p.e.size(); i++)
      if (toLower(p.e[i].name).rfind(lt, 0) == 0) {
        p.cur = (int)i;
        p.clampCursor();
        return true;
      }
    return false;
  }

  void startQS(uint32_t first) {
    if (!cp().isList()) return;
    qsActive = true;
    qs.clear();
    if (first) {
      std::string t = u8encode(first);
      if (qsFind(t)) qs = t;
    }
  }

  bool quickSearchKey(int k) {
    int base = keyBase(k), mods = k & KM_MASK;
    if (k == K_ESC) { qsActive = false; return true; }
    if (k == K_BS) {
      if (!qs.empty()) {
        std::u32string u = toU32(qs);
        u.pop_back();
        qs = fromU32(u);
      } else {
        qsActive = false;
      }
      return true;
    }
    if ((mods == 0 || mods == KM_ALT) && isPrintable(base)) {
      uint32_t c = (uint32_t)base;
      if (uint32_t l = macOptLetter(c)) c = l;
      std::string t = qs + u8encode(c);
      if (qsFind(t)) qs = t;
      return true;
    }
    if (k == K_RESIZE || k == K_NONE || k == K_TIMEOUT) return true;
    qsActive = false;
    return false;
  }

  // ------------------------------------------------------------ mouse

  void mouse() {
    MouseEv m = g_mouse;
    if (m.press && m.button == 0 && m.y == scr.h - 1) {
      int idx = std::min(9, m.x / std::max(1, scr.w / 10));
      handleKey(KF(idx + 1));
      return;
    }
    for (int i = 0; i < 2; i++) {
      Panel& p = P[i];
      if (!p.isList()) continue;
      if (m.x < p.gx || m.x >= p.gx + p.gw || m.y < p.gy || m.y >= p.gy + p.gh) continue;
      act = i;
      if (m.wheelUp) { moveCur(-3); return; }
      if (m.wheelDown) { moveCur(3); return; }
      int idx = p.hit(m.x, m.y);
      if (idx < 0) return;
      if (m.button == 2) { toggleMark(idx, false); return; }
      auto now = std::chrono::steady_clock::now();
      bool dbl = idx == lastClickIdx && i == lastClickPanel && now - lastClick < std::chrono::milliseconds(450);
      p.cur = idx;
      p.clampCursor();
      lastClick = now;
      lastClickIdx = idx;
      lastClickPanel = i;
      if (dbl) {
        lastClickIdx = -1;
        enter();
      }
      return;
    }
  }

  // ------------------------------------------------------------ keys

  void handleKey(int k) {
    if (k == K_NONE || k == K_TIMEOUT || k == K_RESIZE) return;
    if (k == K_MOUSE) { mouse(); return; }
    if (qsActive && quickSearchKey(k)) return;
    bool cmdEmpty = cmd.empty();
    Panel& p = cp();
    int base = keyBase(k), mods = k & KM_MASK;

    // Alt-. (Option-. gives "≥" without Meta): show/hide hidden files
    if (k == ('.' | KM_ALT) || (cmdEmpty && k == 0x2265)) {
      cfg.showHidden = !cfg.showHidden;
      reloadAll();
      return;
    }
    if (cmdEmpty && mods == 0 && isPrintable(k)) {
      if (uint32_t l = macOptLetter((uint32_t)k)) { startQS(l); return; }
    }
    if (mods == KM_ALT && isPrintable(base) && base != '[' && base != 'O' && base != 'b' && base != 'f') {
      startQS((uint32_t)base);
      return;
    }

    switch (k) {
      case K_TAB:
        if (P[1 - act].isList()) act = 1 - act;
        return;
      case K_UP: moveCur(-1); return;
      case K_DOWN: moveCur(1); return;
      case K_PGUP: moveCur(-(p.view == View::Brief ? p.pageSize() : std::max(1, p.listRows() - 1))); return;
      case K_PGDN: moveCur(p.view == View::Brief ? p.pageSize() : std::max(1, p.listRows() - 1)); return;
      case K_HOME:
        if (!cmdEmpty) { cmd.key(k); return; }
        p.cur = 0; p.clampCursor(); return;
      case K_END:
        if (!cmdEmpty) { cmd.key(k); return; }
        p.cur = (int)p.e.size() - 1; p.clampCursor(); return;
      case K_LEFT: case K_RIGHT:
        if (!cmdEmpty) { cmd.key(k); return; }
        if (p.view == View::Brief) moveCur(k == K_LEFT ? -p.listRows() : p.listRows());
        return;
      case K_PGUP | KM_CTRL: case K_UP | KM_CTRL:
        if (p.isList()) goDir(act, parentPath(p.path));
        return;
      case K_PGDN | KM_CTRL: case K_DOWN | KM_CTRL:
        if (p.isList()) enter(true);
        return;
      case 0x1c:  // Ctrl-\ : root
        if (p.isList()) goDir(act, "/");
        return;
      case K_ENTER:
        if (!cmdEmpty) execCommand(cmd.get());
        else if (p.isList()) enter();
        return;
      case K_ENTER | KM_ALT: insertName(true); return;
      case 10: insertName(false); return;  // Ctrl-J (Ctrl-Enter)
      case K_INS: case CTRL('T'): toggleMark(p.cur, true); return;
      case K_ESC: cmd.clear(); return;
      case CTRL('O'): userScreen(); return;
      case CTRL('P'): togglePanel(1 - act); return;
      case CTRL('U'): doAction(A_SWAP); return;
      case CTRL('L'): {
        int o = 1 - act;
        setView(o, P[o].view == View::Info ? P[o].prevView : View::Info);
        if (!P[act].isList() && P[o].isList()) act = o;
        return;
      }
      case CTRL('R'): reloadAll(); return;
      case CTRL('S'): startQS(0); return;
      case CTRL('E'):
        if (hist.empty()) return;
        if (histPos < 0) histPos = (int)hist.size();
        if (histPos > 0) histPos--;
        cmd.set(hist[histPos]);
        return;
      case CTRL('X'):
        if (histPos >= 0 && histPos + 1 < (int)hist.size()) { histPos++; cmd.set(hist[histPos]); }
        else { histPos = -1; cmd.clear(); }
        return;
      case KF(1): help(); return;
      case KF(2): userMenu(); return;
      case KF(3): view(); return;
      case KF(4): editCurrent(); return;
      case KF(5): copyMove(false); return;
      case KF(6): copyMove(true); return;
      case KF(7): makeDir(); return;
      case KF(8): deleteFiles(); return;
      case KF(8) | KM_SHIFT: deleteFiles(true); return;
      case KF(9): doAction(runMenu()); return;
      case KF(10): quit(); return;
      case KF(4) | KM_SHIFT: editNew(); return;
      case KF(6) | KM_SHIFT: copyMove(true, true); return;
      case KF(9) | KM_SHIFT: saveSetup(); return;
      case KF(1) | KM_ALT: driveMenu(0); return;
      case KF(2) | KM_ALT: driveMenu(1); return;
      case KF(7) | KM_ALT: findFile(); return;
      case KF(10) | KM_ALT: treeDialog(); return;
      case KF(8) | KM_ALT: historyDialog(); return;
      case KF(9) | KM_ALT: toggleRows(); return;
      case KF(1) | KM_CTRL: togglePanel(0); return;
      case KF(2) | KM_CTRL: togglePanel(1); return;
      case KF(3) | KM_CTRL: setSort(act, Sort::Name); return;
      case KF(4) | KM_CTRL: setSort(act, Sort::Ext); return;
      case KF(5) | KM_CTRL: setSort(act, Sort::Time); return;
      case KF(6) | KM_CTRL: setSort(act, Sort::Size); return;
      case KF(7) | KM_CTRL: doAction(A_PANEL + act * 100 + P_SUNS); return;
    }
    if (cmdEmpty && p.isList()) {
      if (k == '+') { selectGroup(true); return; }
      if (k == '-') { selectGroup(false); return; }
      if (k == '*') { invertSel(); return; }
    }
    if (cmdEmpty && (k == K_BS || k == K_DEL)) return;
    cmd.key(k);
  }

  // ------------------------------------------------------------ main loop

  void run() {
    while (running) {
      drawAll();
      scr.flush();
      int k = term::readKey(1000);
      if (k == K_TIMEOUT) {
        for (auto& p : P)
          if (p.isList() && p.changedOnDisk()) p.load();
        continue;
      }
      handleKey(k);
    }
  }
};

void usage() {
  printf(
      "ncmac %s - a two-panel file manager for the macOS terminal\n\n"
      "usage: ncmac [options] [leftdir [rightdir]]\n\n"
      "  -s, --size COLSxROWS   resize the terminal window at start (e.g. 80x25)\n"
      "  -l, --lastdir FILE     write the active directory to FILE on exit\n"
      "      --keytest          show which codes your terminal sends for keys\n"
      "  -h, --help             this help\n"
      "  -v, --version          version\n",
      NCMAC_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
  setlocale(LC_ALL, "");
  if (MB_CUR_MAX == 1) setlocale(LC_CTYPE, "en_US.UTF-8");

  std::vector<std::string> dirs;
  std::string lastDirFile;
  int wantW = 0, wantH = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if ((a == "-s" || a == "--size") && i + 1 < argc) {
      if (sscanf(argv[++i], "%dx%d", &wantW, &wantH) != 2) wantW = wantH = 0;
    } else if ((a == "-l" || a == "--lastdir") && i + 1 < argc) {
      lastDirFile = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "--keytest") {
      return term::keyTest();
    } else if (a == "-v" || a == "--version") {
      printf("ncmac %s\n", NCMAC_VERSION);
      return 0;
    } else {
      dirs.push_back(a);
    }
  }

  static App app;
  char cwd[PATH_MAX];
  std::string start = getcwd(cwd, sizeof cwd) ? cwd : homeDir();
  app.loadSetup();
  app.P[0].path = dirs.size() > 0 ? resolvePath(start, dirs[0]) : start;
  app.P[1].path = dirs.size() > 1 ? resolvePath(start, dirs[1]) : app.P[0].path;
  for (auto& p : app.P)
    if (!isDir(p.path)) p.path = start;
  app.loadHistory();

  if (!term::init()) {
    fprintf(stderr, "ncmac: needs an interactive terminal\n");
    return 1;
  }
  term::setMouse(cfg.mouse);
  app.origW = scr.w;
  app.origH = scr.h;
  if (wantW > 0 && wantH > 0) {
    term::requestSize(wantW, wantH);
    app.resizedTerm = true;
  }
  ui::backdrop = [] { app.drawAll(); };
  for (auto& p : app.P) p.load();

  app.run();

  term::shutdown();
  if (app.resizedTerm && (scr.w != app.origW || scr.h != app.origH)) term::requestSize(app.origW, app.origH);
  app.saveHistory();
  if (!app.tmpRoot.empty()) removeTree(app.tmpRoot);
  if (!lastDirFile.empty()) {
    std::ofstream o(lastDirFile);
    o << app.cp().realDir() << "\n";
  }
  return 0;
}
