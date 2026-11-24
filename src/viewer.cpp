// F3 internal viewer: text (wrap / no-wrap) and hex modes, search.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "common.h"

namespace {

const size_t NPOS = SIZE_MAX;

struct Viewer {
  std::string path;
  const unsigned char* d = nullptr;
  size_t n = 0;
  void* map = nullptr;
  bool hex = false, wrap = false;
  size_t top = 0;
  int hcol = 0;
  std::vector<size_t> rows;
  int builtW = -1;
  bool builtWrap = false;
  std::string search;
  size_t mOff = NPOS, mLen = 0;

  ~Viewer() {
    if (map) munmap(map, n);
  }

  bool open() {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      ui::error("Cannot open " + displayPath(path) + ": " + strerror(errno));
      return false;
    }
    struct stat st;
    fstat(fd, &st);
    n = S_ISREG(st.st_mode) ? (size_t)st.st_size : 0;
    if (n > 0) {
      map = mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
      if (map == MAP_FAILED) {
        map = nullptr;
        ::close(fd);
        ui::error("Cannot map " + displayPath(path) + ": " + strerror(errno));
        return false;
      }
      d = (const unsigned char*)map;
    }
    ::close(fd);
    size_t chk = std::min(n, (size_t)8192);
    if (chk && memchr(d, 0, chk)) hex = true;
    return true;
  }

  int W() const { return scr.w; }
  int H() const { return std::max(1, scr.h - 2); }

  size_t count() const { return hex ? std::max<size_t>(1, (n + 15) / 16) : rows.size(); }

  size_t rowOf(size_t off) const {
    auto it = std::upper_bound(rows.begin(), rows.end(), off);
    return it == rows.begin() ? 0 : (size_t)(it - rows.begin() - 1);
  }

  void build() {
    if (builtW == W() && builtWrap == wrap && !rows.empty()) return;
    size_t anchor = (!hex && !rows.empty()) ? rows[std::min(top, rows.size() - 1)] : 0;
    rows.clear();
    rows.push_back(0);
    if (!wrap) {
      for (size_t i = 0; i < n; i++)
        if (d[i] == '\n' && i + 1 < n) rows.push_back(i + 1);
    } else {
      int col = 0, w = W();
      size_t i = 0;
      while (i < n) {
        unsigned char c = d[i];
        if (c == '\n') {
          i++;
          col = 0;
          if (i < n) rows.push_back(i);
          continue;
        }
        size_t st = i;
        int cw;
        if (c == '\t') { cw = 8 - col % 8; i++; }
        else if (c < 32 || c == 127) { cw = (c == '\r') ? 0 : 1; i++; }
        else {
          bool bad = false;
          uint32_t cp = u8decodeBuf(d, n, i, &bad);
          cw = bad ? 1 : cpwidth(cp);
          if (cw < 0) cw = 1;
        }
        if (col + cw > w && col > 0) { rows.push_back(st); col = 0; }
        col += cw;
      }
    }
    builtW = W();
    builtWrap = wrap;
    if (!hex) top = rowOf(anchor);
  }

  void drawText(size_t row, int y) {
    Attr ta = mk(C_LCYAN, C_BLUE), hl = mk(C_BLACK, C_CYAN);
    size_t i = rows[row];
    size_t end = row + 1 < rows.size() ? rows[row + 1] : n;
    int col = 0, w = W();
    while (i < end) {
      unsigned char c = d[i];
      if (c == '\n') break;
      bool inM = mOff != NPOS && i >= mOff && i < mOff + mLen;
      Attr a = inM ? hl : ta;
      if (c == '\t') {
        int t = 8 - col % 8;
        for (int k = 0; k < t; k++, col++)
          if (col >= hcol && col - hcol < w) scr.set(col - hcol, y, " ", a);
        i++;
        continue;
      }
      if (c == '\r' && (i + 1 >= n || d[i + 1] == '\n')) { i++; continue; }
      std::string g;
      int cw;
      if (c < 32 || c == 127) { g = "."; cw = 1; i++; }
      else {
        size_t st = i;
        bool bad = false;
        uint32_t cp = u8decodeBuf(d, n, i, &bad);
        cw = bad ? -1 : cpwidth(cp);
        if (cw < 0) { g = "."; cw = 1; }
        else g.assign((const char*)d + st, i - st);
      }
      if (col >= hcol && col - hcol + cw <= w) scr.put(col - hcol, y, g, a);
      col += cw;
      if (!wrap && col - hcol >= w) break;
    }
  }

  void drawHex(size_t row, int y) {
    Attr ta = mk(C_LCYAN, C_BLUE), hl = mk(C_BLACK, C_CYAN), oa = mk(C_YELLOW, C_BLUE);
    size_t off = row * 16;
    char b[32];
    snprintf(b, sizeof b, n > 0xFFFFFFFFull ? "%010zX" : "%08zX", off);
    int x = scr.put(0, y, b, oa) + 2;
    for (int j = 0; j < 16; j++) {
      size_t p = off + j;
      if (p >= n) break;
      bool inM = mOff != NPOS && p >= mOff && p < mOff + mLen;
      snprintf(b, sizeof b, "%02X", d[p]);
      scr.put(x + j * 3 + (j >= 8 ? 1 : 0), y, b, inM ? hl : ta);
      char ch = (d[p] >= 32 && d[p] < 127) ? (char)d[p] : '.';
      scr.put(x + 16 * 3 + 2 + j, y, std::string(1, ch), inM ? hl : ta);
    }
  }

  void draw() {
    build();
    Attr ta = mk(C_LCYAN, C_BLUE), sa = mk(C_BLACK, C_CYAN);
    scr.clear(ta);
    scr.hideCursor();
    size_t cnt = count();
    int h = H();
    size_t off = hex ? top * 16 : (rows.empty() ? 0 : rows[std::min(top, rows.size() - 1)]);
    int pct = (n == 0 || top + h >= cnt) ? 100 : (int)((double)off * 100 / n);
    std::string right;
    if (hex) {
      char b[40];
      snprintf(b, sizeof b, "Offset %zX", off);
      right = b;
    } else if (!wrap) {
      right = "Line " + std::to_string(top + 1) + "/" + std::to_string(cnt) + "  Col " + std::to_string(hcol);
    }
    right += "  " + fmtThousands(n) + " bytes  " + std::to_string(pct) + "% ";
    scr.fill(0, 0, scr.w, sa);
    int rw = u8width(right);
    scr.put(0, 0, u8fit(" " + displayPath(path), std::max(1, scr.w - rw)), sa);
    scr.put(std::max(0, scr.w - rw), 0, right, sa);
    for (int r = 0; r < h; r++) {
      size_t row = top + r;
      if (row >= cnt) break;
      if (hex) drawHex(row, r + 1);
      else drawText(row, r + 1);
    }
    ui::keybar({"Help", wrap ? "Unwrap" : "Wrap", "Quit", hex ? "Text" : "Hex", "", "", "Search", "", "", "Quit"});
  }

  size_t find(size_t from) const {
    std::string pat = toLower(search);
    size_t m = pat.size();
    if (m == 0 || n < m) return NPOS;
    unsigned char f = (unsigned char)pat[0];
    for (size_t i = from; i + m <= n; i++) {
      if (tolower(d[i]) != f) continue;
      size_t j = 1;
      for (; j < m; j++)
        if (tolower(d[i + j]) != (unsigned char)pat[j]) break;
      if (j == m) return i;
    }
    return NPOS;
  }

  void doSearch(bool ask) {
    if (ask || search.empty()) {
      std::string s = search;
      if (!ui::input("Search", "Search for:", s) || s.empty()) return;
      search = s;
      mOff = NPOS;
    }
    size_t start = (mOff != NPOS) ? mOff + 1 : (hex ? top * 16 : rows[top]);
    size_t f = find(start);
    if (f == NPOS) {
      ui::message("Search", {"\"" + search + "\" not found"}, {"OK"});
      return;
    }
    mOff = f;
    mLen = search.size();
    if (hex) top = f / 16;
    else {
      build();
      top = rowOf(f);
      // make sure the match is horizontally visible in no-wrap mode
      if (!wrap) {
        int col = 0;
        for (size_t i = rows[top]; i < f; i++) col += d[i] == '\t' ? 8 - col % 8 : ((d[i] & 0xC0) != 0x80);
        if (col < hcol || col >= hcol + W()) hcol = std::max(0, col - W() / 2);
      }
    }
  }

  void toggleHex() {
    if (hex) {
      size_t off = top * 16;
      hex = false;
      build();
      top = rowOf(off);
    } else {
      build();
      size_t off = rows[std::min(top, rows.size() - 1)];
      hex = true;
      top = off / 16;
    }
  }

  void help() {
    ui::text("Viewer help",
             {"Up/Down PgUp/PgDn Home/End   scroll", "Left/Right                   scroll horizontally",
              "F2                           wrap / unwrap", "F4                           hex / text",
              "F7  or /                     search", "Shift-F7  or n               search next",
              "Esc F3 F10 q                 quit"});
  }

  void run() {
    ui::BackdropGuard g([this] { draw(); });
    for (;;) {
      draw();
      scr.flush();
      int k = term::readKey(-1);
      size_t cnt = count();
      size_t h = (size_t)H();
      size_t maxTop = cnt > h ? cnt - h : 0;
      switch (k) {
        case K_ESC: case KF(10): case KF(3): case 'q': case 'Q': return;
        case K_DOWN: if (top < maxTop) top++; break;
        case K_UP: if (top > 0) top--; break;
        case K_PGDN: case ' ': top = std::min(maxTop, top + h); break;
        case K_PGUP: case 'b': top = top > h ? top - h : 0; break;
        case K_HOME: case K_HOME | KM_CTRL: top = 0; hcol = 0; break;
        case K_END: case K_END | KM_CTRL: top = maxTop; break;
        case K_LEFT: hcol = std::max(0, hcol - 8); break;
        case K_RIGHT: if (!wrap && !hex) hcol += 8; break;
        case KF(2):
          if (!hex) { wrap = !wrap; hcol = 0; builtW = -1; }
          break;
        case KF(4): toggleHex(); break;
        case KF(7): case '/': doSearch(true); break;
        case KF(7) | KM_SHIFT: case 'n': doSearch(false); break;
        case KF(1): help(); break;
        case K_MOUSE:
          if (g_mouse.wheelUp) top = top > 3 ? top - 3 : 0;
          else if (g_mouse.wheelDown) top = std::min(maxTop, top + 3);
          else if (g_mouse.press && g_mouse.y == scr.h - 1) {
            int idx = std::min(9, g_mouse.x / std::max(1, scr.w / 10));
            if (idx == 9 || idx == 2) return;
            if (idx == 1 && !hex) { wrap = !wrap; hcol = 0; builtW = -1; }
            if (idx == 3) toggleHex();
            if (idx == 6) doSearch(true);
          }
          break;
        case K_RESIZE: builtW = -1; break;
      }
    }
  }
};

}  // namespace

void runViewer(const std::string& path) {
  Viewer v;
  v.path = path;
  if (!v.open()) return;
  v.run();
}
