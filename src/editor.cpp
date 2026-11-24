// F4 internal editor: simple, fast, UTF-8 aware.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwctype>

#include "common.h"

namespace {

struct Editor {
  std::string path;
  std::vector<std::u32string> L;
  size_t cy = 0, cx = 0, top = 0;
  int left = 0, want = 0;
  bool mod = false, crlf = false, finalNL = true;
  std::string search;

  bool load() {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT) { L.assign(1, U""); return true; }
      ui::error("Cannot open " + displayPath(path) + ": " + strerror(errno));
      return false;
    }
    std::string data;
    char buf[65536];
    ssize_t r;
    while ((r = ::read(fd, buf, sizeof buf)) > 0) data.append(buf, (size_t)r);
    ::close(fd);
    if (data.size() > 64u * 1024 * 1024) {
      ui::error("File too large for the built-in editor");
      return false;
    }
    if (data.find('\0') != std::string::npos) {
      int a = ui::message("Edit", {"This looks like a binary file.", "Edit it anyway?"}, {"No", "Yes"}, true);
      if (a != 1) return false;
    }
    L.clear();
    size_t st = 0, lf = 0, crlfN = 0;
    for (size_t i = 0; i <= data.size(); i++) {
      if (i == data.size() || data[i] == '\n') {
        std::string line = data.substr(st, i - st);
        if (i < data.size()) {
          lf++;
          if (!line.empty() && line.back() == '\r') crlfN++;
        }
        L.push_back(toU32(line));
        st = i + 1;
      }
    }
    finalNL = data.empty() || data.back() == '\n';
    if (!data.empty() && data.back() == '\n') L.pop_back();
    if (L.empty()) L.push_back(U"");
    crlf = lf > 0 && crlfN == lf;
    if (crlf)
      for (auto& l : L)
        if (!l.empty() && l.back() == U'\r') l.pop_back();
    return true;
  }

  bool save() {
    std::string out;
    for (size_t i = 0; i < L.size(); i++) {
      out += fromU32(L[i]);
      if (i + 1 < L.size() || finalNL) out += crlf ? "\r\n" : "\n";
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      ui::error("Cannot save " + displayPath(path) + ": " + strerror(errno));
      return false;
    }
    size_t off = 0;
    while (off < out.size()) {
      ssize_t w = ::write(fd, out.data() + off, out.size() - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        ui::error("Write error: " + std::string(strerror(errno)));
        ::close(fd);
        return false;
      }
      off += (size_t)w;
    }
    ::close(fd);
    mod = false;
    return true;
  }

  static int chw(char32_t c, int col) {
    if (c == U'\t') return 8 - col % 8;
    int w = cpwidth((uint32_t)c);
    return w < 0 ? 1 : w;
  }
  static int dispCol(const std::u32string& s, size_t idx) {
    int col = 0;
    for (size_t i = 0; i < idx && i < s.size(); i++) col += chw(s[i], col);
    return col;
  }
  static size_t idxForCol(const std::u32string& s, int target) {
    int col = 0;
    size_t i = 0;
    while (i < s.size()) {
      int w = chw(s[i], col);
      if (col + w > target) break;
      col += w;
      i++;
    }
    return i;
  }

  int H() const { return std::max(1, scr.h - 2); }
  void setWant() { want = dispCol(L[cy], cx); }

  void draw() {
    int W = scr.w, h = H();
    Attr ta = mk(C_WHITE, C_BLUE), sa = mk(C_BLACK, C_CYAN), bad = mk(C_LRED, C_BLUE);
    scr.clear(ta);
    if (cy >= L.size()) cy = L.size() - 1;
    if (cx > L[cy].size()) cx = L[cy].size();
    if (cy < top) top = cy;
    if (cy >= top + (size_t)h) top = cy - h + 1;
    int cc = dispCol(L[cy], cx);
    if (cc < left) left = cc;
    if (cc >= left + W) left = cc - W + 1;
    for (int r = 0; r < h; r++) {
      size_t ln = top + r;
      if (ln >= L.size()) break;
      const auto& s = L[ln];
      int col = 0;
      for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        int w = chw(c, col);
        if (col - left >= W) break;
        if (c == U'\t') {
          col += w;
          continue;
        }
        int x = col - left;
        if (x >= 0 && x + w <= W) {
          if ((c >= 0xDC80 && c <= 0xDCFF) || cpwidth((uint32_t)c) < 0) scr.put(x, r + 1, "?", bad);
          else scr.put(x, r + 1, u8encode((uint32_t)c), ta);
        }
        col += w;
      }
    }
    std::string right = "Line " + std::to_string(cy + 1) + "/" + std::to_string(L.size()) + "  Col " +
                        std::to_string(cc + 1) + (crlf ? "  CRLF " : " ");
    scr.fill(0, 0, W, sa);
    int rw = u8width(right);
    scr.put(0, 0, u8fit(" " + displayPath(path) + (mod ? " *" : ""), std::max(1, W - rw)), sa);
    scr.put(std::max(0, W - rw), 0, right, sa);
    ui::keybar({"Help", "Save", "", "", "", "", "Search", "DelLn", "", "Quit"});
    scr.showCursor(cc - left, 1 + (int)(cy - top));
  }

  void find(bool ask) {
    if (ask || search.empty()) {
      std::string s = search;
      if (!ui::input("Search", "Search for:", s) || s.empty()) return;
      search = s;
    }
    std::u32string pat = toU32(search);
    for (auto& c : pat) c = (char32_t)towlower((wint_t)c);
    auto lower = [](std::u32string s) {
      for (auto& c : s) c = (char32_t)towlower((wint_t)c);
      return s;
    };
    for (size_t ln = cy; ln < L.size(); ln++) {
      std::u32string s = lower(L[ln]);
      size_t from = (ln == cy) ? cx + 1 : 0;
      if (from > s.size()) continue;
      size_t p = s.find(pat, from);
      if (p != std::u32string::npos) {
        cy = ln;
        cx = p;
        setWant();
        return;
      }
    }
    ui::message("Search", {"\"" + search + "\" not found"}, {"OK"});
  }

  void help() {
    ui::text("Editor help",
             {"Arrows, Home/End, PgUp/PgDn  move", "Ctrl-Home / Ctrl-End         start / end of file",
              "Ctrl-Left / Ctrl-Right       word left / right", "Ctrl-Y  or F8                delete line",
              "F2  or Ctrl-S                save", "F7                           search",
              "Shift-F7                     search next", "Esc  or F10                  quit"});
  }

  void deleteLine() {
    if (L.size() > 1) {
      L.erase(L.begin() + (long)cy);
      if (cy >= L.size()) cy = L.size() - 1;
    } else {
      L[0].clear();
    }
    cx = 0;
    mod = true;
  }

  void run() {
    ui::BackdropGuard g([this] { draw(); });
    for (;;) {
      draw();
      scr.flush();
      int k = term::readKey(-1);
      int h = H();
      if (isPrintable(k)) {
        L[cy].insert(cx, 1, (char32_t)k);
        cx++;
        mod = true;
        setWant();
        continue;
      }
      switch (k) {
        case K_ENTER: {
          std::u32string rest = L[cy].substr(cx);
          L[cy].erase(cx);
          L.insert(L.begin() + (long)cy + 1, rest);
          cy++;
          cx = 0;
          mod = true;
          setWant();
          break;
        }
        case K_TAB:
          L[cy].insert(cx, 1, U'\t');
          cx++;
          mod = true;
          setWant();
          break;
        case K_BS:
          if (cx > 0) {
            L[cy].erase(cx - 1, 1);
            cx--;
            mod = true;
          } else if (cy > 0) {
            cx = L[cy - 1].size();
            L[cy - 1] += L[cy];
            L.erase(L.begin() + (long)cy);
            cy--;
            mod = true;
          }
          setWant();
          break;
        case K_DEL:
          if (cx < L[cy].size()) {
            L[cy].erase(cx, 1);
            mod = true;
          } else if (cy + 1 < L.size()) {
            L[cy] += L[cy + 1];
            L.erase(L.begin() + (long)cy + 1);
            mod = true;
          }
          break;
        case CTRL('Y'): case KF(8): deleteLine(); break;
        case K_LEFT:
          if (cx > 0) cx--;
          else if (cy > 0) { cy--; cx = L[cy].size(); }
          setWant();
          break;
        case K_RIGHT:
          if (cx < L[cy].size()) cx++;
          else if (cy + 1 < L.size()) { cy++; cx = 0; }
          setWant();
          break;
        case K_LEFT | KM_CTRL: case K_LEFT | KM_ALT: case 'b' | KM_ALT: {
          auto& s = L[cy];
          if (cx == 0 && cy > 0) { cy--; cx = L[cy].size(); break; }
          while (cx > 0 && !iswalnum((wint_t)s[cx - 1])) cx--;
          while (cx > 0 && iswalnum((wint_t)s[cx - 1])) cx--;
          setWant();
          break;
        }
        case K_RIGHT | KM_CTRL: case K_RIGHT | KM_ALT: case 'f' | KM_ALT: {
          auto& s = L[cy];
          if (cx >= s.size() && cy + 1 < L.size()) { cy++; cx = 0; break; }
          while (cx < s.size() && !iswalnum((wint_t)s[cx])) cx++;
          while (cx < s.size() && iswalnum((wint_t)s[cx])) cx++;
          setWant();
          break;
        }
        case K_UP:
          if (cy > 0) { cy--; cx = idxForCol(L[cy], want); }
          break;
        case K_DOWN:
          if (cy + 1 < L.size()) { cy++; cx = idxForCol(L[cy], want); }
          break;
        case K_PGUP:
          cy = cy > (size_t)h ? cy - h : 0;
          top = top > (size_t)h ? top - h : 0;
          cx = idxForCol(L[cy], want);
          break;
        case K_PGDN:
          cy = std::min(L.size() - 1, cy + h);
          top = std::min(L.size() - 1, top + h);
          cx = idxForCol(L[cy], want);
          break;
        case K_HOME: case CTRL('A'): cx = 0; setWant(); break;
        case K_END: case CTRL('E'): cx = L[cy].size(); setWant(); break;
        case K_HOME | KM_CTRL: case K_PGUP | KM_CTRL: cy = 0; cx = 0; setWant(); break;
        case K_END | KM_CTRL: case K_PGDN | KM_CTRL: cy = L.size() - 1; cx = L[cy].size(); setWant(); break;
        case KF(2): case CTRL('S'): save(); break;
        case KF(7): find(true); break;
        case KF(7) | KM_SHIFT: find(false); break;
        case KF(1): help(); break;
        case K_MOUSE:
          if (g_mouse.wheelUp) { top = top > 3 ? top - 3 : 0; if (cy >= top + h) cy = top + h - 1; }
          else if (g_mouse.wheelDown) {
            if (top + 3 < L.size()) top += 3;
            if (cy < top) cy = top;
          } else if (g_mouse.press && g_mouse.y >= 1 && g_mouse.y <= h) {
            size_t ln = top + (size_t)(g_mouse.y - 1);
            if (ln < L.size()) { cy = ln; cx = idxForCol(L[cy], g_mouse.x + left); setWant(); }
          } else if (g_mouse.press && g_mouse.y == scr.h - 1) {
            int idx = std::min(9, g_mouse.x / std::max(1, scr.w / 10));
            if (idx == 1) save();
            else if (idx == 6) find(true);
            else if (idx == 9) goto quit;
          }
          cx = std::min(cx, L[cy].size());
          break;
        case KF(10): case K_ESC:
        quit: {
          if (!mod) return;
          int r = ui::message("Edit", {"The file has been modified.", "Do you want to save it?"},
                              {"Yes", "No", "Cancel"});
          if (r == 0) {
            if (save()) return;
          } else if (r == 1) {
            return;
          }
          break;
        }
      }
    }
  }
};

}  // namespace

void runEditor(const std::string& path) {
  Editor e;
  e.path = path;
  if (!e.load()) return;
  e.run();
}
