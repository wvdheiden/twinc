#include <algorithm>
#include <cctype>

#include "common.h"

namespace ui {

std::function<void()> backdrop = [] { scr.clear(mk(C_DEFAULT, C_DEFAULT)); };

// ------------------------------------------------------------ LineEdit

void LineEdit::set(const std::string& v) {
  s = toU32(v);
  pos = s.size();
  scroll = 0;
}

std::string LineEdit::get() const { return fromU32(s); }

static bool isWordCh(char32_t c) { return c == '_' || c >= 128 || isalnum((int)c); }

bool LineEdit::key(int k) {
  if (isPrintable(k)) {
    s.insert(pos, 1, (char32_t)k);
    pos++;
    return true;
  }
  switch (k) {
    case K_BS:
      if (pos > 0) { s.erase(pos - 1, 1); pos--; }
      return true;
    case K_DEL:
      if (pos < s.size()) s.erase(pos, 1);
      return true;
    case K_LEFT: if (pos > 0) pos--; return true;
    case K_RIGHT: if (pos < s.size()) pos++; return true;
    case K_HOME: case CTRL('A'): pos = 0; return true;
    case K_END: pos = s.size(); return true;
    case CTRL('Y'): clear(); return true;
    case CTRL('K'): s.erase(pos); return true;
    case CTRL('W'): case K_BS | KM_ALT: {
      size_t p = pos;
      while (p > 0 && !isWordCh(s[p - 1])) p--;
      while (p > 0 && isWordCh(s[p - 1])) p--;
      s.erase(p, pos - p);
      pos = p;
      return true;
    }
    case K_LEFT | KM_CTRL: case K_LEFT | KM_ALT: case 'b' | KM_ALT:
      while (pos > 0 && !isWordCh(s[pos - 1])) pos--;
      while (pos > 0 && isWordCh(s[pos - 1])) pos--;
      return true;
    case K_RIGHT | KM_CTRL: case K_RIGHT | KM_ALT: case 'f' | KM_ALT:
      while (pos < s.size() && !isWordCh(s[pos])) pos++;
      while (pos < s.size() && isWordCh(s[pos])) pos++;
      return true;
  }
  return false;
}

void LineEdit::draw(int x, int y, int w, Attr a, bool focus) {
  if (w <= 0) return;
  if (pos > s.size()) pos = s.size();
  if (scroll > pos) scroll = pos;
  auto widthRange = [&](size_t from, size_t to) {
    int t = 0;
    for (size_t i = from; i < to; i++) {
      int c = cpwidth(s[i]);
      t += c < 0 ? 1 : c;
    }
    return t;
  };
  while (scroll < pos && widthRange(scroll, pos) > w - 1) scroll++;
  std::string vis;
  for (size_t i = scroll; i < s.size(); i++) {
    if (s[i] >= 0xDC80 && s[i] <= 0xDCFF) vis += '?';
    else vis += u8encode(s[i]);
  }
  scr.fill(x, y, w, a);
  scr.put(x, y, vis, a, w);
  if (focus) scr.showCursor(x + widthRange(scroll, pos), y);
}

// ------------------------------------------------------------ drawing helpers

Attr dlgAttr(bool warn) { return warn ? mk(C_WHITE, C_RED) : mk(C_BLACK, C_LGRAY); }

void frame(int x, int y, int w, int h, Attr a, const std::string& title, bool shadow) {
  scr.rect(x, y, w, h, a);
  scr.box(x + 1, y, w - 2, h, a, true);
  if (!title.empty()) {
    std::string t = " " + u8trunc(title, w - 8) + " ";
    scr.put(x + (w - u8width(t)) / 2, y, t, a);
  }
  if (shadow) {
    for (int r = y + 1; r <= y + h; r++) { scr.shade(x + w, r); scr.shade(x + w + 1, r); }
    for (int c = x + 2; c < x + w; c++) scr.shade(c, y + h);
  }
}

void keybar(const std::vector<std::string>& labels) {
  int y = scr.h - 1;
  int sw = scr.w / 10;
  if (sw < 3) sw = 3;
  for (int i = 0; i < 10; i++) {
    int x = i * sw;
    if (x >= scr.w) break;
    int wdt = (i == 9) ? scr.w - x : sw;
    std::string num = std::to_string(i + 1);
    scr.put(x, y, num, mk(C_WHITE, C_BLACK));
    int lw = wdt - (int)num.size();
    std::string lab = i < (int)labels.size() ? labels[i] : "";
    scr.put(x + (int)num.size(), y, u8fit(lab, lw, false), mk(C_BLACK, C_CYAN));
  }
}

// ------------------------------------------------------------ message box

int message(const std::string& title, const std::vector<std::string>& lines,
            const std::vector<std::string>& buttons, bool warn, int def) {
  int sel = def;
  for (;;) {
    backdrop();
    Attr a = dlgAttr(warn);
    int bw = 0;
    for (auto& b : buttons) bw += u8width(b) + 5;
    int cw = std::max(bw, u8width(title) + 6);
    for (auto& l : lines) cw = std::max(cw, u8width(l));
    int w = std::min(cw + 8, scr.w - 2);
    int h = (int)lines.size() + 4 + (buttons.empty() ? 0 : 1);
    int x = (scr.w - w) / 2, y = std::max(0, (scr.h - h) / 2 - 1);
    frame(x, y, w, h, a, title);
    for (size_t i = 0; i < lines.size(); i++)
      scr.put(x + 3, y + 1 + (int)i + 1, u8center(lines[i], w - 6), a);
    int by = y + h - 2;
    int bx = x + (w - bw) / 2 + 1;
    std::vector<int> bpos;
    for (size_t i = 0; i < buttons.size(); i++) {
      std::string t = "[ " + buttons[i] + " ]";
      Attr ba = ((int)i == sel) ? mk(C_BLACK, C_CYAN) : a;
      bpos.push_back(bx);
      scr.put(bx, by, t, ba);
      if (!buttons[i].empty() && (int)i != sel)
        scr.put(bx + 2, by, buttons[i].substr(0, 1), warn ? mk(C_YELLOW, C_RED) : mk(C_RED, C_LGRAY));
      bx += u8width(t) + 1;
    }
    scr.hideCursor();
    scr.flush();
    int k = term::readKey(-1);
    int n = (int)buttons.size();
    if (k == K_RESIZE || k == K_NONE || k == K_TIMEOUT) continue;
    if (n == 0) return -1;
    if (k == K_ESC || k == KF(10)) return -1;
    if (k == K_ENTER || k == ' ') return sel;
    if (k == K_LEFT || k == (K_TAB | KM_SHIFT)) { sel = (sel + n - 1) % n; continue; }
    if (k == K_RIGHT || k == K_TAB) { sel = (sel + 1) % n; continue; }
    if (k == K_MOUSE && g_mouse.press && g_mouse.y == by) {
      for (int i = 0; i < n; i++)
        if (g_mouse.x >= bpos[i] && g_mouse.x < bpos[i] + u8width(buttons[i]) + 4) return i;
      continue;
    }
    if (isPrintable(k) && k < 128) {
      for (int i = 0; i < n; i++)
        if (!buttons[i].empty() && tolower(buttons[i][0]) == tolower(k)) return i;
    }
  }
}

void error(const std::string& msg) { message("Error", {msg}, {"OK"}, true); }

// ------------------------------------------------------------ input box

bool input(const std::string& title, const std::string& prompt, std::string& val,
           std::vector<std::string>* hist) {
  LineEdit le;
  le.set(val);
  int hp = -1;
  for (;;) {
    backdrop();
    Attr a = dlgAttr(false);
    int w = std::min(scr.w - 4, std::max(66, u8width(prompt) + 8));
    int h = 6;
    int x = (scr.w - w) / 2, y = std::max(0, (scr.h - h) / 2 - 2);
    frame(x, y, w, h, a, title);
    scr.put(x + 3, y + 2, u8fit(prompt, w - 6), a);
    le.draw(x + 3, y + 3, w - 6, mk(C_BLACK, C_CYAN), true);
    scr.flush();
    int k = term::readKey(-1);
    if (k == K_RESIZE || k == K_NONE || k == K_TIMEOUT) continue;
    if (k == K_ENTER) {
      val = le.get();
      if (hist && !trim(val).empty()) {
        auto it = std::find(hist->begin(), hist->end(), val);
        if (it != hist->end()) hist->erase(it);
        hist->push_back(val);
      }
      return true;
    }
    if (k == K_ESC || k == KF(10)) return false;
    if (hist && !hist->empty() && (k == K_UP || k == K_DOWN)) {
      int n = (int)hist->size();
      if (k == K_UP) hp = (hp < 0) ? n - 1 : std::max(0, hp - 1);
      else hp = (hp < 0) ? -1 : (hp + 1 < n ? hp + 1 : -1);
      le.set(hp < 0 ? "" : (*hist)[hp]);
      continue;
    }
    le.key(k);
  }
}

// ------------------------------------------------------------ list box

int list(const std::string& title, const std::vector<std::string>& items, int sel, bool hotkeys) {
  int top = 0;
  int n = (int)items.size();
  if (n == 0) return -1;
  for (;;) {
    backdrop();
    Attr a = mk(C_BLACK, C_CYAN);
    Attr hl = mk(C_WHITE, C_BLACK);
    int maxw = u8width(title) + 8;
    for (auto& it : items) maxw = std::max(maxw, u8width(it) + 6);
    int w = std::min(maxw, scr.w - 4);
    int h = std::min(n + 2, scr.h - 3);
    if (h < 3) h = 3;
    int rows = h - 2;
    int x = (scr.w - w) / 2, y = std::max(0, (scr.h - h) / 2 - 1);
    sel = std::max(0, std::min(sel, n - 1));
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    frame(x, y, w, h, a, title);
    for (int r = 0; r < rows; r++) {
      int i = top + r;
      if (i >= n) break;
      scr.put(x + 2, y + 1 + r, " " + u8fit(items[i], w - 6) + " ", i == sel ? hl : a);
    }
    if (top > 0) scr.put(x + w - 2, y + 1, "▲", a);
    if (top + rows < n) scr.put(x + w - 2, y + h - 2, "▼", a);
    scr.hideCursor();
    scr.flush();
    int k = term::readKey(-1);
    switch (k) {
      case K_UP: sel = (sel + n - 1) % n; break;
      case K_DOWN: sel = (sel + 1) % n; break;
      case K_PGUP: sel = std::max(0, sel - rows); break;
      case K_PGDN: sel = std::min(n - 1, sel + rows); break;
      case K_HOME: sel = 0; break;
      case K_END: sel = n - 1; break;
      case K_ENTER: return sel;
      case K_ESC: case KF(10): return -1;
      case K_MOUSE:
        if (g_mouse.wheelUp) sel = std::max(0, sel - 3);
        else if (g_mouse.wheelDown) sel = std::min(n - 1, sel + 3);
        else if (g_mouse.press) {
          int r = g_mouse.y - (y + 1);
          if (g_mouse.x > x && g_mouse.x < x + w - 1 && r >= 0 && r < rows && top + r < n) return top + r;
          if (g_mouse.x < x || g_mouse.x >= x + w || g_mouse.y < y || g_mouse.y >= y + h) return -1;
        }
        break;
      default:
        if (isPrintable(k)) {
          uint32_t c = (uint32_t)k;
          if (c < 128) c = (uint32_t)tolower((int)c);
          for (int d = 1; d <= n; d++) {
            int j = (sel + d) % n;
            std::string t = trim(items[j]);
            if (t.empty()) continue;
            size_t ii = 0;
            uint32_t f = u8decode(t, ii);
            if (f < 128) f = (uint32_t)tolower((int)f);
            if (f == c) {
              sel = j;
              if (hotkeys) return j;
              break;
            }
          }
        }
    }
  }
}

// ------------------------------------------------------------ text viewer box (help)

void text(const std::string& title, const std::vector<std::string>& lines) {
  int top = 0;
  for (;;) {
    backdrop();
    Attr a = mk(C_BLACK, C_CYAN);
    int maxw = 0;
    for (auto& l : lines) maxw = std::max(maxw, u8width(l));
    int w = std::min(scr.w - 4, maxw + 6);
    int h = std::min(scr.h - 2, (int)lines.size() + 2);
    int x = (scr.w - w) / 2, y = (scr.h - h) / 2;
    int rows = h - 2;
    int maxTop = std::max(0, (int)lines.size() - rows);
    top = std::max(0, std::min(top, maxTop));
    frame(x, y, w, h, a, title);
    for (int r = 0; r < rows && top + r < (int)lines.size(); r++)
      scr.put(x + 3, y + 1 + r, u8trunc(lines[top + r], w - 6), a);
    if (top > 0) scr.put(x + w - 2, y + 1, "▲", a);
    if (top < maxTop) scr.put(x + w - 2, y + h - 2, "▼", a);
    scr.hideCursor();
    scr.flush();
    int k = term::readKey(-1);
    switch (k) {
      case K_UP: top--; break;
      case K_DOWN: top++; break;
      case K_PGUP: top -= rows; break;
      case K_PGDN: case ' ': top += rows; break;
      case K_HOME: top = 0; break;
      case K_END: top = maxTop; break;
      case K_MOUSE:
        if (g_mouse.wheelUp) top -= 3;
        else if (g_mouse.wheelDown) top += 3;
        else return;
        break;
      case K_ESC: case K_ENTER: case KF(10): case KF(1): case 'q': return;
    }
  }
}

// ------------------------------------------------------------ progress

void progress(const std::string& title, const std::vector<std::string>& lines, double f1, double f2) {
  backdrop();
  Attr a = dlgAttr(false);
  int w = std::min(scr.w - 4, 70);
  int bars = (f1 >= 0) + (f2 >= 0);
  int h = (int)lines.size() + 3 + bars;
  int x = (scr.w - w) / 2, y = std::max(0, (scr.h - h) / 2 - 1);
  frame(x, y, w, h, a, title);
  int r = y + 2;
  for (auto& l : lines) scr.put(x + 3, r++, u8truncLeft(l, w - 6), a);
  auto bar = [&](double f) {
    f = std::max(0.0, std::min(1.0, f));
    int bw = w - 12;
    int filled = (int)(f * bw + 0.5);
    std::string s;
    for (int i = 0; i < bw; i++) s += i < filled ? "█" : "░";
    scr.put(x + 3, r, s, mk(C_CYAN, C_LGRAY));
    scr.put(x + 4 + bw, r, u8rjust(std::to_string((int)(f * 100)) + "%", 4), a);
    r++;
  };
  if (f1 >= 0) bar(f1);
  if (f2 >= 0) bar(f2);
  scr.hideCursor();
  scr.flush();
}

}  // namespace ui
