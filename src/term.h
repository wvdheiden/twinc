// Terminal handling: raw mode, key decoding, double-buffered screen.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum : uint8_t {
  C_BLACK = 0, C_RED, C_GREEN, C_BROWN, C_BLUE, C_MAGENTA, C_CYAN, C_LGRAY,
  C_DGRAY, C_LRED, C_LGREEN, C_YELLOW, C_LBLUE, C_LMAGENTA, C_LCYAN, C_WHITE,
  C_DEFAULT = 255
};

struct Attr {
  uint8_t fg = C_DEFAULT, bg = C_DEFAULT;
};
inline bool operator==(Attr a, Attr b) { return a.fg == b.fg && a.bg == b.bg; }
inline bool operator!=(Attr a, Attr b) { return !(a == b); }
inline Attr mk(uint8_t fg, uint8_t bg) { Attr a; a.fg = fg; a.bg = bg; return a; }

// ---- keys ----
enum : int {
  K_TIMEOUT = -2,
  K_NONE = -1,
  K_SPECIAL = 0x110000,
  K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL,
  K_ENTER, K_TAB, K_BS, K_ESC, K_RESIZE, K_MOUSE,
  K_F1 = K_SPECIAL + 0x100,
};
constexpr int KM_SHIFT = 0x1000000, KM_ALT = 0x2000000, KM_CTRL = 0x4000000;
constexpr int KM_MASK = KM_SHIFT | KM_ALT | KM_CTRL;
constexpr int KF(int n) { return K_F1 + n - 1; }
constexpr int keyBase(int k) { return k < 0 ? k : (k & ~KM_MASK); }
inline bool isPrintable(int k) { return k >= 32 && k < K_SPECIAL && k != 127; }
constexpr int CTRL(char c) { return c & 0x1f; }

struct MouseEv {
  int x = 0, y = 0, button = 0;
  bool press = false, wheelUp = false, wheelDown = false;
};
extern MouseEv g_mouse;

// ---- UTF-8 helpers ----
uint32_t u8decodeBuf(const unsigned char* s, size_t len, size_t& i, bool* bad = nullptr);
uint32_t u8decode(const std::string& s, size_t& i);
std::string u8encode(uint32_t cp);
int cpwidth(uint32_t cp);  // -1 = non printable
int u8width(const std::string& s);
std::string u8trunc(const std::string& s, int w, bool marker = true);
std::string u8truncLeft(const std::string& s, int w);
std::string u8fit(const std::string& s, int w, bool marker = true);  // truncate + pad
std::string u8rjust(const std::string& s, int w);
std::string u8center(const std::string& s, int w);
std::u32string toU32(const std::string& s);   // invalid bytes -> U+DC80..U+DCFF
std::string fromU32(const std::u32string& s);

// ---- screen buffer ----
class Screen {
 public:
  int w = 0, h = 0;
  int curX = 0, curY = 0;
  bool cursorOn = false;

  void resize(int W, int H);
  void clear(Attr a);
  void set(int x, int y, const std::string& ch, Attr a, uint8_t wid = 1);
  int put(int x, int y, const std::string& s, Attr a, int maxw = -1);
  void fill(int x, int y, int n, Attr a, const std::string& ch = " ");
  void rect(int x, int y, int W, int H, Attr a);
  void box(int x, int y, int W, int H, Attr a, bool dbl = true);
  void shade(int x, int y);  // recolor for drop shadows
  void hideCursor() { cursorOn = false; }
  void showCursor(int x, int y) { curX = x; curY = y; cursorOn = true; }
  void flush();
  void invalidate() { full = true; }

 private:
  struct Cell {
    std::string ch = " ";
    Attr a;
    uint8_t wid = 1;
  };
  std::vector<Cell> buf, old;
  bool full = true;
  Cell* at(int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return nullptr;
    return &buf[(size_t)y * w + x];
  }
};
extern Screen scr;

namespace term {
bool init();
void shutdown();
void suspend();             // cooked mode + normal screen (for running programs)
void resume();              // raw mode + alternate screen
void setRaw(bool on);
void altScreen(bool on);
void setMouse(bool on);
int readKey(int timeoutMs);  // handles SIGWINCH itself (resizes scr) -> K_RESIZE
void size(int& w, int& h);
void requestSize(int cols, int rows);
void write(const std::string& s);
int keyTest();  // twinc --keytest
std::string keyName(int k);
}  // namespace term
