#include "term.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <signal.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwchar>

Screen scr;
MouseEv g_mouse;

#ifdef _WIN32
// The Windows console runs in VT mode: output understands the same escape
// sequences and keys arrive as xterm sequences (ENABLE_VIRTUAL_TERMINAL_INPUT).
static HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;
static DWORD g_origIn = 0, g_origOut = 0;
static UINT g_origCP = 0, g_origOutCP = 0;
#else
static termios g_orig;
#endif
static bool g_haveOrig = false, g_isAlt = false, g_mouseOn = false;
static volatile sig_atomic_t g_resized = 0;
#ifndef _WIN32
static void onWinch(int) { g_resized = 1; }
#endif
static std::string ib;  // pending input bytes

// ============================================================ UTF-8

uint32_t u8decodeBuf(const unsigned char* s, size_t len, size_t& i, bool* bad) {
  if (bad) *bad = false;
  unsigned char c = s[i];
  if (c < 0x80) { i++; return c; }
  int n;
  if (c >= 0xC2 && c <= 0xDF) n = 1;
  else if (c >= 0xE0 && c <= 0xEF) n = 2;
  else if (c >= 0xF0 && c <= 0xF4) n = 3;
  else { i++; if (bad) *bad = true; return 0xFFFD; }
  if (i + n >= len) { i++; if (bad) *bad = true; return 0xFFFD; }
  uint32_t cp = c & (0x3F >> n);
  for (int k = 1; k <= n; k++) {
    unsigned char d = s[i + k];
    if ((d & 0xC0) != 0x80) { i++; if (bad) *bad = true; return 0xFFFD; }
    cp = (cp << 6) | (d & 0x3F);
  }
  i += n + 1;
  return cp;
}

uint32_t u8decode(const std::string& s, size_t& i) {
  return u8decodeBuf((const unsigned char*)s.data(), s.size(), i);
}

std::string u8encode(uint32_t cp) {
  std::string r;
  if (cp >= 0xDC80 && cp <= 0xDCFF) { r += (char)(cp - 0xDC00); return r; }
  if (cp < 0x80) r += (char)cp;
  else if (cp < 0x800) {
    r += (char)(0xC0 | (cp >> 6));
    r += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    r += (char)(0xE0 | (cp >> 12));
    r += (char)(0x80 | ((cp >> 6) & 0x3F));
    r += (char)(0x80 | (cp & 0x3F));
  } else {
    r += (char)(0xF0 | (cp >> 18));
    r += (char)(0x80 | ((cp >> 12) & 0x3F));
    r += (char)(0x80 | ((cp >> 6) & 0x3F));
    r += (char)(0x80 | (cp & 0x3F));
  }
  return r;
}

int cpwidth(uint32_t cp) {
  if (cp < 32 || cp == 127) return -1;
  if (cp < 127) return 1;
  if (cp < 0xA0) return -1;
  if (cp >= 0xDC80 && cp <= 0xDCFF) return -1;
  // invisible format characters (BOM, zero-width space/joiners): terminals give them no cell
  if (cp == 0xFEFF || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064)) return 0;
#ifdef _WIN32
  // no wcwidth(): combining marks are 0 wide, East Asian wide/fullwidth and emoji 2
  static const uint32_t zero[][2] = {{0x300, 0x36F}, {0x483, 0x489}, {0x591, 0x5BD}, {0x610, 0x61A},
                                     {0x64B, 0x65F}, {0x200B, 0x200F}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
                                     {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF}};
  static const uint32_t wide[][2] = {{0x1100, 0x115F}, {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x25FD, 0x25FE},
                                     {0x2614, 0x2615}, {0x2648, 0x2653}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
                                     {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F5},
                                     {0x26FA, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
                                     {0x274C, 0x274C}, {0x2753, 0x2755}, {0x2795, 0x2797}, {0x27B0, 0x27B0},
                                     {0x2B1B, 0x2B1C}, {0x2B50, 0x2B55}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
                                     {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xAC00, 0xD7A3},
                                     {0xF900, 0xFAFF}, {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
                                     {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
                                     {0x1F191, 0x1F19A}, {0x1F200, 0x1F2FF}, {0x1F300, 0x1F64F},
                                     {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
                                     {0x20000, 0x3FFFD}};
  for (auto& r : zero)
    if (cp >= r[0] && cp <= r[1]) return 0;
  for (auto& r : wide)
    if (cp >= r[0] && cp <= r[1]) return 2;
  return 1;
#else
  int w = wcwidth((wchar_t)cp);
  if (w < 0) return 1;
  return w;
#endif
}

int u8width(const std::string& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    int c = cpwidth(u8decode(s, i));
    w += c < 0 ? 1 : c;
  }
  return w;
}

std::string u8trunc(const std::string& s, int w, bool marker) {
  if (w <= 0) return "";
  if (u8width(s) <= w) return s;
  int lim = marker ? w - 1 : w;
  std::string r;
  int cw = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t st = i;
    int c = cpwidth(u8decode(s, i));
    if (c < 0) c = 1;
    if (cw + c > lim) break;
    r.append(s, st, i - st);
    cw += c;
  }
  if (marker) r += "\xE2\x80\xA6";  // …
  return r;
}

std::string u8truncLeft(const std::string& s, int w) {
  if (w <= 0) return "";
  if (u8width(s) <= w) return s;
  std::u32string u = toU32(s);
  int cw = 0;
  size_t start = u.size();
  while (start > 0) {
    int c = cpwidth(u[start - 1]);
    if (c < 0) c = 1;
    if (cw + c > w - 1) break;
    cw += c;
    start--;
  }
  return "\xE2\x80\xA6" + fromU32(u.substr(start));
}

std::string u8fit(const std::string& s, int w, bool marker) {
  std::string r = u8trunc(s, w, marker);
  int rw = u8width(r);
  if (rw < w) r.append((size_t)(w - rw), ' ');
  return r;
}

std::string u8rjust(const std::string& s, int w) {
  std::string r = u8trunc(s, w, false);
  int rw = u8width(r);
  if (rw < w) r.insert(0, (size_t)(w - rw), ' ');
  return r;
}

std::string u8center(const std::string& s, int w) {
  std::string r = u8trunc(s, w, true);
  int rw = u8width(r);
  int l = (w - rw) / 2;
  return std::string((size_t)std::max(0, l), ' ') + r +
         std::string((size_t)std::max(0, w - rw - l), ' ');
}

std::u32string toU32(const std::string& s) {
  std::u32string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    size_t st = i;
    bool bad = false;
    uint32_t cp = u8decodeBuf((const unsigned char*)s.data(), s.size(), i, &bad);
    if (bad) { cp = 0xDC00 + (unsigned char)s[st]; i = st + 1; }
    r.push_back((char32_t)cp);
  }
  return r;
}

std::string fromU32(const std::u32string& s) {
  std::string r;
  for (char32_t c : s) r += u8encode((uint32_t)c);
  return r;
}

// ============================================================ Screen

void Screen::resize(int W, int H) {
  w = std::max(W, 1);
  h = std::max(H, 1);
  buf.assign((size_t)w * h, Cell());
  old.assign((size_t)w * h, Cell());
  full = true;
}

void Screen::clear(Attr a) {
  for (auto& c : buf) { c.ch = " "; c.a = a; c.wid = 1; }
}

void Screen::set(int x, int y, const std::string& ch, Attr a, uint8_t wid) {
  Cell* c = at(x, y);
  if (!c) return;
  c->ch = ch;
  c->a = a;
  c->wid = wid;
}

int Screen::put(int x, int y, const std::string& s, Attr a, int maxw) {
  if (y < 0 || y >= h) return 0;
  int limit = maxw < 0 ? w : std::min(w, x + maxw);
  int cx = x;
  size_t i = 0;
  while (i < s.size()) {
    size_t st = i;
    uint32_t cp = u8decode(s, i);
    int cw = cpwidth(cp);
    if (cw == 0) {  // combining mark: attach to previous cell
      if (cx > 0) {
        Cell* p = at(cx - 1, y);
        if (p && p->ch.empty()) p = at(cx - 2, y);
        if (p) p->ch.append(s, st, i - st);
      }
      continue;
    }
    std::string g;
    if (cw < 0) { g = "?"; cw = 1; }
    else g = s.substr(st, i - st);
    if (cx + cw > limit) break;
    if (cx >= 0) {
      set(cx, y, g, a, (uint8_t)cw);
      if (cw == 2) set(cx + 1, y, "", a, 0);
    }
    cx += cw;
  }
  return cx - x;
}

void Screen::fill(int x, int y, int n, Attr a, const std::string& ch) {
  for (int i = 0; i < n; i++) set(x + i, y, ch, a);
}

void Screen::rect(int x, int y, int W, int H, Attr a) {
  for (int r = 0; r < H; r++) fill(x, y + r, W, a);
}

void Screen::box(int x, int y, int W, int H, Attr a, bool dbl) {
  if (W < 2 || H < 2) return;
  const char* tl = dbl ? "╔" : "┌";
  const char* tr = dbl ? "╗" : "┐";
  const char* bl = dbl ? "╚" : "└";
  const char* br = dbl ? "╝" : "┘";
  const char* hz = dbl ? "═" : "─";
  const char* vt = dbl ? "║" : "│";
  set(x, y, tl, a);
  set(x + W - 1, y, tr, a);
  set(x, y + H - 1, bl, a);
  set(x + W - 1, y + H - 1, br, a);
  fill(x + 1, y, W - 2, a, hz);
  fill(x + 1, y + H - 1, W - 2, a, hz);
  for (int r = 1; r < H - 1; r++) {
    set(x, y + r, vt, a);
    set(x + W - 1, y + r, vt, a);
  }
}

void Screen::shade(int x, int y) {
  Cell* c = at(x, y);
  if (c) c->a = mk(C_DGRAY, C_BLACK);
}

static void sgr(std::string& out, Attr a) {
  out += "\x1b[0;";
  if (a.fg == C_DEFAULT) out += "39";
  else out += std::to_string(a.fg < 8 ? 30 + a.fg : 90 + a.fg - 8);
  out += ';';
  if (a.bg == C_DEFAULT) out += "49";
  else out += std::to_string(a.bg < 8 ? 40 + a.bg : 100 + a.bg - 8);
  out += 'm';
}

void Screen::flush() {
  std::string out;
  out.reserve((size_t)w * h * 3);
  out += "\x1b[?25l";
  Attr cur;
  bool curValid = false;
  for (int y = 0; y < h; y++) {
    size_t base = (size_t)y * w;
    if (!full) {
      bool same = true;
      for (int x = 0; x < w; x++) {
        const Cell& a = buf[base + x];
        const Cell& b = old[base + x];
        if (a.ch != b.ch || a.a != b.a) { same = false; break; }
      }
      if (same) continue;
    }
    out += "\x1b[" + std::to_string(y + 1) + ";1H";
    bool prevWide = false;
    for (int x = 0; x < w; x++) {
      const Cell& c = buf[base + x];
      if (!curValid || c.a != cur) { sgr(out, c.a); cur = c.a; curValid = true; }
      if (c.ch.empty()) {
        if (prevWide) { prevWide = false; continue; }
        out += ' ';
        continue;
      }
      if (c.wid == 2) {
        if (x + 1 < w && buf[base + x + 1].ch.empty()) { out += c.ch; prevWide = true; }
        else { out += ' '; prevWide = false; }
        continue;
      }
      out += c.ch;
      prevWide = false;
    }
  }
  old = buf;
  full = false;
  out += "\x1b[0m";
  if (cursorOn) {
    out += "\x1b[" + std::to_string(curY + 1) + ";" + std::to_string(curX + 1) + "H\x1b[?25h";
  }
  term::write(out);
}

// ============================================================ terminal

namespace term {

void write(const std::string& s) {
#ifdef _WIN32
  size_t off = 0;
  while (off < s.size()) {
    DWORD n = 0;
    if (!WriteFile(g_out != INVALID_HANDLE_VALUE ? g_out : GetStdHandle(STD_OUTPUT_HANDLE), s.data() + off,
                   (DWORD)std::min<size_t>(s.size() - off, 1 << 20), &n, nullptr) || n == 0)
      return;
    off += n;
  }
  return;
#else
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(1, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    off += (size_t)n;
  }
#endif
}

void setRaw(bool on) {
  if (!g_haveOrig) return;
#ifdef _WIN32
  if (on) {
    SetConsoleMode(g_in, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    SetConsoleMode(g_out, g_origOut | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                              DISABLE_NEWLINE_AUTO_RETURN);
  } else {
    SetConsoleMode(g_in, g_origIn);
    SetConsoleMode(g_out, g_origOut | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
  return;
#else
  if (on) {
    termios r = g_orig;
    r.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    r.c_oflag &= ~(tcflag_t)(OPOST);
    r.c_cflag |= CS8;
    r.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &r);
  } else {
    tcsetattr(0, TCSANOW, &g_orig);
  }
#endif
}

void altScreen(bool on) {
  if (on) {
    write("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[H\x1b[2J");
    if (g_mouseOn) write("\x1b[?1000h\x1b[?1006h");
    scr.invalidate();
  } else {
    write("\x1b[0m\x1b[?1000l\x1b[?1006l\x1b[?7h\x1b[?25h\x1b[?1049l");
  }
  g_isAlt = on;
}

void setMouse(bool on) {
  g_mouseOn = on;
  if (g_isAlt) write(on ? "\x1b[?1000h\x1b[?1006h" : "\x1b[?1000l\x1b[?1006l");
}

void size(int& w, int& h) {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (GetConsoleScreenBufferInfo(g_out, &ci)) {
    w = ci.srWindow.Right - ci.srWindow.Left + 1;
    h = ci.srWindow.Bottom - ci.srWindow.Top + 1;
  } else {
    w = 80;
    h = 25;
  }
  return;
#else
  winsize ws{};
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    w = ws.ws_col;
    h = ws.ws_row;
  } else {
    w = 80;
    h = 25;
  }
#endif
}

void requestSize(int cols, int rows) {
  write("\x1b[8;" + std::to_string(rows) + ";" + std::to_string(cols) + "t");
}

#ifdef _WIN32
static bool winInit() {
  g_in = GetStdHandle(STD_INPUT_HANDLE);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!GetConsoleMode(g_in, &g_origIn) || !GetConsoleMode(g_out, &g_origOut)) return false;
  g_origCP = GetConsoleCP();
  g_origOutCP = GetConsoleOutputCP();
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  g_haveOrig = true;
  return true;
}
#endif

bool init() {
#ifdef _WIN32
  if (!winInit()) return false;
  setRaw(true);
  altScreen(true);
  int w, h;
  size(w, h);
  scr.resize(w, h);
  return true;
#else
  if (!isatty(0) || !isatty(1)) return false;
  tcgetattr(0, &g_orig);
  g_haveOrig = true;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = onWinch;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: poll() must be interrupted
  sigaction(SIGWINCH, &sa, nullptr);
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  setRaw(true);
  altScreen(true);
  int w, h;
  size(w, h);
  scr.resize(w, h);
  return true;
#endif
}

void shutdown() {
  if (g_isAlt) altScreen(false);
  setRaw(false);
#ifdef _WIN32
  if (g_haveOrig) {
    SetConsoleMode(g_out, g_origOut);
    SetConsoleCP(g_origCP);
    SetConsoleOutputCP(g_origOutCP);
  }
#endif
}

void suspend() {
  altScreen(false);
  setRaw(false);
}

void resume() {
  setRaw(true);
  altScreen(true);
  ib.clear();
}

// ---------------------------------------------------------------- input

static bool fillBuf(int timeout) {
  if (g_resized) return false;
#ifdef _WIN32
  // Wait for console input; keep only typed characters (VT input turns special keys
  // into escape sequences) and note window size changes.
  ULONGLONG until = timeout < 0 ? 0 : GetTickCount64() + (ULONGLONG)timeout;
  for (;;) {
    DWORD wait = INFINITE;
    if (timeout >= 0) {
      ULONGLONG now = GetTickCount64();
      wait = now >= until ? 0 : (DWORD)(until - now);
    }
    if (WaitForSingleObject(g_in, wait) != WAIT_OBJECT_0) return false;
    INPUT_RECORD rec[128];
    DWORD n = 0;
    if (!ReadConsoleInputW(g_in, rec, 128, &n)) return false;
    std::wstring w;
    for (DWORD i = 0; i < n; i++) {
      if (rec[i].EventType == KEY_EVENT) {
        const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
        if (k.bKeyDown && k.uChar.UnicodeChar)
          for (WORD r = 0; r < std::max<WORD>(k.wRepeatCount, 1); r++) w += k.uChar.UnicodeChar;
      } else if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
        g_resized = 1;
      }
    }
    if (!w.empty()) {
      int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
      std::string b((size_t)len, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &b[0], len, nullptr, nullptr);
      ib += b;
      return true;
    }
    if (g_resized) return false;
    if (timeout == 0) return false;
  }
#else
  struct pollfd p;
  p.fd = 0;
  p.events = POLLIN;
  p.revents = 0;
  int r = poll(&p, 1, timeout);
  if (r <= 0) return false;
  char b[1024];
  ssize_t n = ::read(0, b, sizeof b);
  if (n == 0) { shutdown(); _exit(0); }  // terminal went away
  if (n < 0) return false;
  ib.append(b, (size_t)n);
  return true;
#endif
}

static int take(size_t n, int key) {
  ib.erase(0, std::min(n, ib.size()));
  return key;
}

static int modBits(int m) {
  m -= 1;
  if (m <= 0) return 0;
  int r = 0;
  if (m & 1) r |= KM_SHIFT;
  if (m & 2) r |= KM_ALT;
  if (m & 4) r |= KM_CTRL;
  if (m & 8) r |= KM_ALT;
  return r;
}

static int tildeKey(int n) {
  switch (n) {
    case 1: case 7: return K_HOME;
    case 2: return K_INS;
    case 3: return K_DEL;
    case 4: case 8: return K_END;
    case 5: return K_PGUP;
    case 6: return K_PGDN;
    case 11: return KF(1);
    case 12: return KF(2);
    case 13: return KF(3);
    case 14: return KF(4);
    case 15: return KF(5);
    case 17: return KF(6);
    case 18: return KF(7);
    case 19: return KF(8);
    case 20: return KF(9);
    case 21: return KF(10);
    case 23: return KF(11);
    case 24: return KF(12);
    // rxvt / Terminal.app style shifted function keys
    case 25: return KF(3) | KM_SHIFT;
    case 26: return KF(4) | KM_SHIFT;
    case 28: return KF(5) | KM_SHIFT;
    case 29: return KF(6) | KM_SHIFT;
    case 31: return KF(7) | KM_SHIFT;
    case 32: return KF(8) | KM_SHIFT;
    case 33: return KF(9) | KM_SHIFT;
    case 34: return KF(10) | KM_SHIFT;
  }
  return K_NONE;
}

static int parse();

static int parseCSI() {
  for (int tries = 0;; tries++) {
    size_t i = 2;
    bool mouse = false;
    if (i < ib.size() && ib[i] == '<') { mouse = true; i++; }
    size_t ps = i;
    while (i < ib.size() && ((ib[i] >= '0' && ib[i] <= '9') || ib[i] == ';')) i++;
    if (i < ib.size()) {
      char fin = ib[i];
      std::vector<int> p;
      int v = 0;
      bool have = false;
      for (size_t j = ps; j < i; j++) {
        if (ib[j] == ';') { p.push_back(v); v = 0; have = false; }
        else { v = v * 10 + (ib[j] - '0'); have = true; }
      }
      if (have || i > ps) p.push_back(v);
      size_t len = i + 1;
      if (mouse) {
        if ((fin == 'M' || fin == 'm') && p.size() >= 3) {
          int b = p[0];
          g_mouse = MouseEv();
          g_mouse.x = p[1] - 1;
          g_mouse.y = p[2] - 1;
          if (b & 64) {
            if (fin == 'm') return take(len, K_NONE);
            if (b & 1) g_mouse.wheelDown = true;
            else g_mouse.wheelUp = true;
            return take(len, K_MOUSE);
          }
          if (b & 32) return take(len, K_NONE);
          g_mouse.button = b & 3;
          g_mouse.press = (fin == 'M');
          if (!g_mouse.press) return take(len, K_NONE);
          return take(len, K_MOUSE);
        }
        return take(len, K_NONE);
      }
      int mod = p.size() >= 2 ? modBits(p[1]) : 0;
      int k = K_NONE;
      switch (fin) {
        case 'A': k = K_UP; break;
        case 'B': k = K_DOWN; break;
        case 'C': k = K_RIGHT; break;
        case 'D': k = K_LEFT; break;
        case 'H': k = K_HOME; break;
        case 'F': k = K_END; break;
        case 'P': k = KF(1); break;
        case 'Q': k = KF(2); break;
        case 'R': k = KF(3); break;
        case 'S': k = KF(4); break;
        case 'Z': k = K_TAB | KM_SHIFT; break;
        case '~': k = tildeKey(p.empty() ? 0 : p[0]); break;
      }
      if (k == K_NONE) return take(len, K_NONE);
      return take(len, k | mod);
    }
    if (tries >= 3 || !fillBuf(50)) return take(2, KM_ALT | '[');
  }
}

static int parse() {
  unsigned char c = (unsigned char)ib[0];
  if (c == 27) {
    if (ib.size() == 1) fillBuf(40);
    if (ib.size() == 1) return take(1, K_ESC);
    unsigned char d = (unsigned char)ib[1];
    if (d == '[') return parseCSI();
    if (d == 'O') {
      if (ib.size() < 3) fillBuf(40);
      if (ib.size() < 3) return take(2, KM_ALT | 'O');
      int k = K_NONE;
      switch (ib[2]) {
        case 'P': k = KF(1); break;
        case 'Q': k = KF(2); break;
        case 'R': k = KF(3); break;
        case 'S': k = KF(4); break;
        case 'A': k = K_UP; break;
        case 'B': k = K_DOWN; break;
        case 'C': k = K_RIGHT; break;
        case 'D': k = K_LEFT; break;
        case 'H': k = K_HOME; break;
        case 'F': k = K_END; break;
        case 'M': k = K_ENTER; break;
        case 'k': k = '+'; break;
        case 'm': k = '-'; break;
        case 'j': k = '*'; break;
      }
      return take(3, k);
    }
    if (d == 27) {
      // Option-as-Meta + special key: ESC ESC [ ...
      if (ib.size() > 2 && (ib[2] == '[' || ib[2] == 'O')) {
        ib.erase(0, 1);
        int k = parse();
        return k >= 0 ? (k | KM_ALT) : k;
      }
      return take(2, K_ESC);
    }
    if (d >= '0' && d <= '9') return take(2, KF(d == '0' ? 10 : d - '0'));  // mc-style Esc+digit
    if (d == '\r') return take(2, K_ENTER | KM_ALT);
    if (d == 127 || d == 8) return take(2, K_BS | KM_ALT);
    if (d == '\t') return take(2, K_TAB | KM_ALT);
    if (d < 32) return take(2, d | KM_ALT);
    ib.erase(0, 1);
    int k = parse();
    return k >= 0 ? (k | KM_ALT) : k;
  }
  if (c == 127 || c == 8) return take(1, K_BS);
  if (c == '\r') return take(1, K_ENTER);
  if (c == '\t') return take(1, K_TAB);
  if (c < 32) return take(1, c);
  size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (ib.size() < need) fillBuf(20);
  size_t i = 0;
  bool bad = false;
  uint32_t cp = u8decodeBuf((const unsigned char*)ib.data(), ib.size(), i, &bad);
  ib.erase(0, i);
  return bad ? K_NONE : (int)cp;
}

static int readKeyRaw(int timeout);

// Ctrl-F prefix for keyboards without function keys (iPad):
// Ctrl-F 1..9,0 = F1..F10; s/a/c before the digit adds Shift/Alt/Ctrl.
static int fkeyPrefix() {
  int mod = 0;
  for (;;) {
    if (scr.h > 0) {
      Attr a = mk(C_BLACK, C_YELLOW);
      std::string m = mod == KM_SHIFT ? "Shift-" : mod == KM_ALT ? "Alt-" : mod == KM_CTRL ? "Ctrl-" : "";
      std::string t = " " + m + "F-key: 1..9,0 = F1..F10   s/a/c = Shift/Alt/Ctrl   Esc = cancel";
      scr.fill(0, scr.h - 1, scr.w, a);
      scr.put(0, scr.h - 1, t, a);
      scr.flush();
    }
    int k = readKeyRaw(-1);
    if (k == K_NONE || k == K_TIMEOUT || k == K_RESIZE) continue;
    if (k >= '0' && k <= '9') return KF(k == '0' ? 10 : k - '0') | mod;
    if (k == 's' || k == 'S') mod = KM_SHIFT;
    else if (k == 'a' || k == 'A') mod = KM_ALT;
    else if (k == 'c' || k == 'C') mod = KM_CTRL;
    else return K_NONE;
  }
}

int readKey(int timeout) {
  int k = readKeyRaw(timeout);
  if (k == CTRL('F')) return fkeyPrefix();
  return k;
}

static int readKeyRaw(int timeout) {
  auto resized = []() {
    g_resized = 0;
    int w, h;
    size(w, h);
    scr.resize(w, h);
    return K_RESIZE;
  };
  if (g_resized) return resized();
  if (ib.empty()) {
    if (!fillBuf(timeout)) {
      if (g_resized) return resized();
      return K_TIMEOUT;
    }
  }
  return parse();
}

std::string keyName(int k) {
  if (k < 0) return k == K_NONE ? "(ignored)" : "(timeout)";
  std::string m;
  if (k & KM_CTRL) m += "Ctrl-";
  if (k & KM_ALT) m += "Alt-";
  if (k & KM_SHIFT) m += "Shift-";
  int b = keyBase(k);
  if (b >= K_F1 && b < K_F1 + 12) return m + "F" + std::to_string(b - K_F1 + 1);
  static const char* names[] = {"Up", "Down", "Left", "Right", "Home", "End", "PgUp", "PgDn", "Insert",
                                "Delete", "Enter", "Tab", "Backspace", "Esc", "Resize", "Mouse"};
  if (b > K_SPECIAL && b <= K_MOUSE) return m + names[b - K_SPECIAL - 1];
  if (b < 32) return m + "Ctrl-" + std::string(1, (char)('@' + b));
  return m + "'" + u8encode((uint32_t)b) + "'";
}

int keyTest() {
#ifdef _WIN32
  if (!winInit()) return 1;
#else
  if (!isatty(0)) return 1;
  tcgetattr(0, &g_orig);
  g_haveOrig = true;
#endif
  setRaw(true);
  write("twinc key test: press keys to see what the terminal sends and how twinc reads them.\r\n"
        "Press q twice to quit.\r\n\r\n");
  int qs = 0;
  for (;;) {
    std::string chunk;
    if (!fillBuf(-1)) continue;
    while (fillBuf(30)) {}
    chunk = ib;
    std::string esc;
    for (unsigned char c : chunk) {
      if (c == 27) esc += "ESC ";
      else if (c < 32 || c == 127) { char b[8]; snprintf(b, sizeof b, "0x%02X ", c); esc += b; }
      else esc += (char)c;
    }
    std::string dec;
    while (!ib.empty()) dec += (dec.empty() ? "" : ", ") + keyName(parse());
    write("  " + esc + std::string(esc.size() < 24 ? 24 - esc.size() : 1, ' ') + "->  " + dec + "\r\n");
    if (chunk == "q") { if (++qs == 2) break; } else qs = 0;
  }
  setRaw(false);
  return 0;
}

}  // namespace term
