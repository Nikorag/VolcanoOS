#include "osk.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util.hpp"
#include "widgets.hpp"

namespace vos {

namespace {
// Four letter rows plus a command row (SHIFT / SPACE / DELETE / DONE).
const char* kRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-@"};
constexpr int kCmdRow = 4;
const char* kCmds[4] = {"SHIFT", "SPACE", "DELETE", "DONE"};

std::string keyAt(int r, int c, bool shift) {
  char ch = kRows[r][c];
  if (shift && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
  return std::string(1, ch);
}
}  // namespace

void Osk::show(const std::string& title, const std::string& initial, size_t maxLen,
               std::function<void(const std::string&)> done) {
  open_ = true;
  title_ = title;
  text_ = initial;
  maxLen_ = maxLen;
  done_ = std::move(done);
  row_ = 1;
  col_ = 0;
  shift_ = false;
  openedAt_ = nowSeconds();
}

void Osk::typeText(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 32 || c == 127) continue;
    if (text_.size() < maxLen_) text_ += static_cast<char>(c);
  }
}

void Osk::backspace() {
  while (!text_.empty()) {
    bool cont = (static_cast<unsigned char>(text_.back()) & 0xC0) == 0x80;
    text_.pop_back();
    if (!cont) break;
  }
}

void Osk::submit() {
  open_ = false;
  auto cb = done_;
  if (cb) cb(text_);
}

bool Osk::handle(Action a) {
  if (!open_) return false;
  switch (a) {
    case Action::Up: row_ = (row_ + kCmdRow) % (kCmdRow + 1); break;
    case Action::Down: row_ = (row_ + 1) % (kCmdRow + 1); break;
    case Action::Left: {
      int n = row_ == kCmdRow ? 4 : 10;
      col_ = (col_ + n - 1) % n;
      break;
    }
    case Action::Right: {
      int n = row_ == kCmdRow ? 4 : 10;
      col_ = (col_ + 1) % n;
      break;
    }
    case Action::Accept:
      if (row_ == kCmdRow) {
        if (col_ == 0) shift_ = !shift_;
        else if (col_ == 1) typeText(" ");
        else if (col_ == 2) backspace();
        else submit();
      } else {
        typeText(keyAt(row_, col_, shift_));
        shift_ = false;
      }
      break;
    case Action::X: backspace(); break;
    case Action::Y: shift_ = !shift_; break;
    case Action::Start: submit(); break;
    case Action::Back: cancel(); break;
    default: break;
  }
  if (row_ == kCmdRow) col_ = std::min(col_, 3);
  return true;
}

void Osk::draw(Gfx& g, PadStyle style, double now) {
  if (!open_) return;
  const float W = g.W(), H = g.H();
  float k = ui::easeOut(static_cast<float>((now - openedAt_) / 0.16));
  g.rect(0, 0, W, H, Color(4, 3, 3, static_cast<uint8_t>(205 * k)));

  const float cell = 76, gap = 10;
  float gridW = 10 * cell + 9 * gap;
  float pw = gridW + 80, ph = 150 + 5 * (cell + gap) + 70;
  float px = W / 2 - pw / 2, py = H / 2 - ph / 2 + (1 - k) * 20;
  g.rrect(px, py, pw, ph, 12, Color(18, 14, 12, static_cast<uint8_t>(252 * k)));
  g.rrectLine(px, py, pw, ph, 12, 1.5f, theme::line.alpha(k));
  ui::cornerTicks(g, px - 6, py - 6, pw + 12, ph + 12, 24, 2, theme::magma.alpha(0.8f * k));

  TextStyle t;
  t.font = Font::Head;
  t.bold = true;
  t.size = 26;
  t.tracking = 5;
  t.color = theme::text.alpha(k);
  g.text(title_, px + 40, py + 28, t);

  // Text field with a blinking caret.
  float fy = py + 82, fh = 56;
  g.rrect(px + 40, fy, pw - 80, fh, 6, Color(8, 6, 6, 255));
  g.rrectLine(px + 40, fy, pw - 80, fh, 6, 1.5f, theme::magma.alpha(k));
  TextStyle f;
  f.size = 26;
  f.color = theme::text.alpha(k);
  f.tracking = 1;
  std::string shown = text_;
  float tw = g.measure(shown, f).x;
  float maxW = pw - 130;
  while (tw > maxW && !shown.empty()) {  // keep the tail visible
    shown.erase(0, 1);
    tw = g.measure(shown, f).x;
  }
  g.text(shown, px + 58, fy + (fh - g.measure("A", f).y) / 2, f);
  if (std::fmod(now, 1.0) < 0.6) g.rect(px + 60 + tw, fy + 12, 3, fh - 24, theme::magma);

  float gy = fy + fh + 28, gx = px + 40;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 10; ++c) {
      float x = gx + c * (cell + gap), y = gy + r * (cell + gap);
      bool on = (row_ == r && col_ == c);
      if (on) g.glow(x + cell / 2, y + cell / 2, cell, cell, theme::magma.alpha(0.25f));
      g.rrect(x, y, cell, cell, 8, on ? Color(255, 106, 31, 255) : theme::panelHi);
      TextStyle kt;
      kt.font = Font::Head;
      kt.bold = true;
      kt.size = 30;
      kt.align = Align::Center;
      kt.color = on ? Color(20, 10, 6) : theme::text;
      std::string label = keyAt(r, c, shift_);
      g.text(label, x + cell / 2, y + (cell - g.measure(label, kt).y) / 2, kt);
    }
  }
  // Command row: four wide keys.
  float cw = (gridW - 3 * gap) / 4;
  for (int c = 0; c < 4; ++c) {
    float x = gx + c * (cw + gap), y = gy + 4 * (cell + gap);
    bool on = (row_ == kCmdRow && col_ == c);
    bool lit = (c == 0 && shift_) || c == 3;
    if (on) g.glow(x + cw / 2, y + cell / 2, cw * 0.7f, cell, theme::magma.alpha(0.25f));
    g.rrect(x, y, cw, cell, 8, on ? Color(255, 106, 31, 255) : lit ? Color(70, 40, 24) : theme::panelHi);
    TextStyle kt;
    kt.font = Font::Head;
    kt.bold = true;
    kt.size = 22;
    kt.tracking = 4;
    kt.align = Align::Center;
    kt.color = on ? Color(20, 10, 6) : theme::text;
    g.text(kCmds[c], x + cw / 2, y + (cell - g.measure("A", kt).y) / 2, kt);
  }

  float hy = py + ph - 52, hx = px + 40;
  hx += ui::prompt(g, style, Action::Accept, "TYPE", hx, hy, 22, k) + 30;
  hx += ui::prompt(g, style, Action::X, "DELETE", hx, hy, 22, k) + 30;
  hx += ui::prompt(g, style, Action::Y, "SHIFT", hx, hy, 22, k) + 30;
  hx += ui::prompt(g, style, Action::Start, "DONE", hx, hy, 22, k) + 30;
  ui::prompt(g, style, Action::Back, "CANCEL", hx, hy, 22, k);
}

}  // namespace vos
