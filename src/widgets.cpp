#include "widgets.hpp"

#include <algorithm>
#include <cmath>

namespace vos::ui {

float easeOut(float t) {
  t = std::min(1.f, std::max(0.f, t));
  return 1 - (1 - t) * (1 - t) * (1 - t);
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }

Color tint(const std::string& slug) {
  uint32_t h = 2166136261u;
  for (unsigned char c : slug) h = (h ^ c) * 16777619u;
  // Warm family only, so the shelf reads as one volcano rather than a rainbow.
  static const Color palette[] = {Color(255, 106, 31), Color(255, 160, 60), Color(226, 70, 40),
                                  Color(240, 120, 90), Color(214, 150, 60), Color(255, 90, 110)};
  return palette[h % (sizeof palette / sizeof palette[0])];
}

void cornerTicks(Gfx& g, float x, float y, float w, float h, float len, float thick, Color c) {
  g.rect(x, y, len, thick, c);
  g.rect(x, y, thick, len, c);
  g.rect(x + w - len, y, len, thick, c);
  g.rect(x + w - thick, y, thick, len, c);
  g.rect(x, y + h - thick, len, thick, c);
  g.rect(x, y + h - len, thick, len, c);
  g.rect(x + w - len, y + h - thick, len, thick, c);
  g.rect(x + w - thick, y + h - len, thick, len, c);
}

namespace {

struct GlyphSpec {
  Color color;
  const char* letter;   // text glyph (Xbox/Nintendo/keyboard)
  int shape;            // 0 = letter, 1 cross, 2 circle, 3 square, 4 triangle
};

GlyphSpec faceGlyph(PadStyle s, Action a) {
  if (s == PadStyle::PlayStation) {
    switch (a) {
      case Action::Accept: return {Color(122, 162, 255), "", 1};
      case Action::Back: return {Color(255, 107, 107), "", 2};
      case Action::X: return {Color(255, 139, 209), "", 3};
      default: return {Color(94, 224, 160), "", 4};
    }
  }
  switch (a) {
    case Action::Accept: return {Color(126, 231, 135), "A", 0};
    case Action::Back: return {Color(255, 123, 114), "B", 0};
    case Action::X: return {Color(121, 192, 255), "X", 0};
    default: return {Color(242, 204, 96), "Y", 0};
  }
}

bool isFace(Action a) { return a == Action::Accept || a == Action::Back || a == Action::X || a == Action::Y; }

const char* keyLabel(Action a) {
  switch (a) {
    case Action::Accept: return "ENTER";
    case Action::Back: return "ESC";
    case Action::X: return "X";
    case Action::Y: return "R";
    case Action::LB: return "Q";
    case Action::RB: return "E";
    case Action::Start: return "TAB";
    case Action::Select: return "-";
    case Action::Home: return "F10";
    default: return "ARROWS";
  }
}

const char* padLabel(PadStyle s, Action a) {
  bool ps = s == PadStyle::PlayStation;
  switch (a) {
    case Action::LB: return ps ? "L1" : "LB";
    case Action::RB: return ps ? "R1" : "RB";
    case Action::Start: return ps ? "OPTIONS" : "MENU";
    case Action::Select: return ps ? "SHARE" : "VIEW";
    case Action::Home: return ps ? "PS" : "HOME";
    default: return "";
  }
}

// Returns the glyph's width, drawing it when `draw`.
float glyph(Gfx& g, PadStyle style, Action a, float x, float y, float size, float alpha, bool draw) {
  TextStyle ts;
  ts.font = Font::Head;
  ts.bold = true;
  ts.size = size * 0.78f;

  if (style == PadStyle::Keyboard || (!isFace(a) && std::string(padLabel(style, a)).empty())) {
    std::string lbl = keyLabel(a);
    ts.font = Font::Mono;
    ts.size = size * 0.62f;
    ts.tracking = 0.4f;
    float tw = g.measure(lbl, ts).x;
    float w = std::max(size * 1.15f, tw + size * 0.7f);
    if (draw) {
      g.rrect(x, y, w, size * 1.15f, 5, theme::panelHi.alpha(alpha));
      g.rrectLine(x, y, w, size * 1.15f, 5, 1.5f, theme::dim.alpha(alpha));
      ts.color = theme::text.alpha(alpha);
      g.text(lbl, x + w / 2, y + (size * 1.15f - g.measure(lbl, ts).y) / 2, [&] { auto t = ts; t.align = Align::Center; return t; }());
    }
    return w;
  }

  if (isFace(a)) {
    GlyphSpec spec = faceGlyph(style, a);
    float d = size * 1.15f, cx = x + d / 2, cy = y + d / 2;
    if (draw) {
      g.circle(cx, cy, d / 2, theme::panelHi.alpha(alpha));
      g.ring(cx, cy, d / 2, 1.8f, spec.color.alpha(alpha));
      Color c = spec.color.alpha(alpha);
      float r = d * 0.22f;
      switch (spec.shape) {
        case 0: {
          ts.color = c;
          ts.align = Align::Center;
          ts.size = size * 0.72f;
          g.text(spec.letter, cx, cy - g.measure(spec.letter, ts).y / 2, ts);
          break;
        }
        case 1:
          g.line(cx - r, cy - r, cx + r, cy + r, 2.4f, c);
          g.line(cx - r, cy + r, cx + r, cy - r, 2.4f, c);
          break;
        case 2: g.ring(cx, cy, r * 1.15f, 2.4f, c); break;
        case 3: g.rrectLine(cx - r, cy - r, r * 2, r * 2, 1, 2.4f, c); break;
        case 4: {
          float h = r * 1.25f;
          g.line(cx, cy - h, cx + h, cy + h * 0.75f, 2.4f, c);
          g.line(cx + h, cy + h * 0.75f, cx - h, cy + h * 0.75f, 2.4f, c);
          g.line(cx - h, cy + h * 0.75f, cx, cy - h, 2.4f, c);
          break;
        }
      }
    }
    return d;
  }

  // Shoulders, menu buttons: a pill with a short label.
  std::string lbl = padLabel(style, a);
  ts.font = Font::Mono;
  ts.size = size * 0.58f;
  ts.tracking = 0.4f;
  float tw = g.measure(lbl, ts).x;
  float w = tw + size * 0.9f, h = size * 1.05f;
  if (draw) {
    g.rrect(x, y + size * 0.05f, w, h, h / 2, theme::panelHi.alpha(alpha));
    g.rrectLine(x, y + size * 0.05f, w, h, h / 2, 1.5f, theme::dim.alpha(alpha));
    ts.color = theme::text.alpha(alpha);
    ts.align = Align::Center;
    g.text(lbl, x + w / 2, y + size * 0.05f + (h - g.measure(lbl, ts).y) / 2, ts);
  }
  return w;
}

}  // namespace

// Button prompts are no longer shown anywhere (the UI is driven by focus alone).
float promptWidth(Gfx&, PadStyle, Action, const std::string&, float) { return 0; }

float prompt(Gfx&, PadStyle, Action, const std::string&, float, float, float, float) { return 0; }

float chip(Gfx& g, const std::string& text, float x, float y, Color fg, Color border, float size, Color fill) {
  TextStyle ts;
  ts.size = size;
  ts.tracking = 0.6f;
  ts.color = fg;
  Vec2 m = g.measure(text, ts);
  float w = m.x + size * 1.3f, h = size * 1.9f;
  if (fill.a) g.rrect(x, y, w, h, 3, fill);
  g.rrectLine(x, y, w, h, 3, 1.4f, border);
  g.text(text, x + size * 0.65f, y + (h - m.y) / 2, ts);
  return w;
}

void glass(Gfx& g, float x, float y, float w, float h, float r, float alpha, float fill) {
  g.rrect(x, y, w, h, r, Color(28, 22, 19, static_cast<uint8_t>(255 * fill * alpha)));
  g.rrectLine(x, y, w, h, r, 1.2f, theme::line.alpha(alpha));
}

void focusRing(Gfx& g, float x, float y, float w, float h, float r, float alpha) {
  g.rrectLine(x - 3, y - 3, w + 6, h + 6, r + 3, 3, Color(255, 255, 255, static_cast<uint8_t>(255 * alpha)));
}

void toggleSwitch(Gfx& g, float x, float y, bool on, float alpha) {
  const float w = 68, h = 38;
  g.rrect(x, y, w, h, h / 2, on ? theme::magma.alpha(alpha) : Color(247, 241, 236, static_cast<uint8_t>(46 * alpha)));
  g.circle(x + (on ? w - h / 2 : h / 2), y + h / 2, 15, Color(255, 255, 255, static_cast<uint8_t>(255 * alpha)));
}

void spinner(Gfx& g, float cx, float cy, float r, double now) {
  g.ring(cx, cy, r, 3, theme::magma.alpha(0.3f));
  g.arc(cx, cy, r, 3, static_cast<float>(now) * 6, static_cast<float>(now) * 6 + 1.6f, theme::magma);
}

void icon(Gfx& g, Icon ic, float cx, float cy, float size, Color c) {
  const float u = size / 24.f, t = std::max(2.f, 2.1f * u);
  auto L = [&](float x1, float y1, float x2, float y2) { g.line(cx + (x1 - 12) * u, cy + (y1 - 12) * u, cx + (x2 - 12) * u, cy + (y2 - 12) * u, t, c); };
  auto A = [&](float x, float y, float r, float a0, float a1) { g.arc(cx + (x - 12) * u, cy + (y - 12) * u, r * u, t, a0, a1, c); };
  const float PI = 3.14159265f;
  switch (ic) {
    case Icon::Trophy:
      g.rrectLine(cx - 6 * u, cy - 10 * u, 12 * u, 13 * u, 5 * u, t, c);
      L(12, 15, 12, 19); L(8, 21, 16, 21);
      A(5, 6.5f, 2.5f * 1.0f, PI * 0.5f, PI * 1.5f); A(19, 6.5f, 2.5f, -PI * 0.5f, PI * 0.5f);
      break;
    case Icon::Users:
      g.ring(cx - 3 * u, cy - 5 * u, 3.6f * u, t, c); A(9, 21, 6.5f, PI * 1.05f, PI * 1.95f);
      A(16, 8, 3.2f, -PI * 0.5f, PI * 0.5f); A(17, 20, 4.5f, PI * 1.15f, PI * 1.6f);
      break;
    case Icon::User:
      g.ring(cx, cy - 5 * u, 4 * u, t, c); A(12, 22, 7.5f, PI * 1.05f, PI * 1.95f);
      break;
    case Icon::Refresh:
      A(12, 12, 8.5f, -PI * 0.1f, PI * 1.35f);
      L(21, 4, 21, 9); L(21, 9, 16, 9);
      A(12, 12, 8.5f, PI * 0.9f, PI * 2.35f);
      L(3, 20, 3, 15); L(3, 15, 8, 15);
      break;
    case Icon::Wifi:
      for (int i = 0; i < 3; ++i) A(12, 20, 5.5f + i * 5.5f, -PI * 0.78f, -PI * 0.22f);
      g.circle(cx, cy + 8 * u, 1.6f * u, c);
      break;
    case Icon::Bluetooth:
      L(7, 7, 17, 17); L(17, 17, 12, 22); L(12, 22, 12, 2); L(12, 2, 17, 7); L(17, 7, 7, 17);
      break;
    case Icon::Expand:
      L(3, 8, 3, 3); L(3, 3, 8, 3); L(16, 3, 21, 3); L(21, 3, 21, 8);
      L(3, 16, 3, 21); L(3, 21, 8, 21); L(21, 16, 21, 21); L(21, 21, 16, 21);
      break;
    case Icon::SignOut:
      L(9, 21, 5, 21); L(5, 21, 3, 19); L(3, 19, 3, 5); L(3, 5, 5, 3); L(5, 3, 9, 3);
      L(21, 12, 9, 12); L(21, 12, 16, 7); L(21, 12, 16, 17);
      break;
    case Icon::Power:
      L(12, 2, 12, 12); A(12, 13, 8.5f, -PI * 0.5f + 0.75f, -PI * 0.5f + 2 * PI - 0.75f);
      break;
    case Icon::Clock:
      g.ring(cx, cy, 9 * u, t, c); L(12, 7, 12, 12); L(12, 12, 15.5f, 14);
      break;
    case Icon::Gamepad:
      g.rrectLine(cx - 10 * u, cy - 6 * u, 20 * u, 12 * u, 5 * u, t, c);
      L(6, 12, 10, 12); L(8, 10, 8, 14); g.circle(cx + 3 * u, cy + 1 * u, 1.2f * u, c); g.circle(cx + 6 * u, cy - 1 * u, 1.2f * u, c);
      break;
    case Icon::Plus: L(12, 5, 12, 19); L(5, 12, 19, 12); break;
    case Icon::Lock:
      g.rrect(cx - 6.5f * u, cy - 1 * u, 13 * u, 10.5f * u, 2.5f * u, c); A(12, 11, 4.5f, PI, PI * 2);
      break;
    case Icon::Restart:
      A(12, 12, 8, PI * 0.15f, PI * 1.8f); L(20, 3, 20, 8); L(20, 8, 15, 8);
      break;
    case Icon::Quit:
      L(6, 6, 18, 18); L(18, 6, 6, 18);
      break;
  }
}

void progressBar(Gfx& g, float x, float y, float w, float h, float frac, Color fill, Color track) {
  frac = std::min(1.f, std::max(0.f, frac));
  g.rrect(x, y, w, h, h / 2, track);
  if (frac > 0) {
    float fw = std::max(h, w * frac);
    g.rrectGrad(x, y, fw, h, h / 2, Color::mix(fill, Color(255, 255, 255), 0.25f), fill);
  }
}

void button(Gfx& g, float x, float y, float w, float h, const std::string& label, Color accent, bool enabled, double t) {
  float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(t) * 3.2f);
  if (enabled) {
    g.glow(x + w / 2, y + h / 2, w * 0.85f, h * 1.6f, accent.alpha(0.30f + 0.12f * pulse));
    g.rrectGrad(x, y, w, h, 6, Color::mix(accent, Color(255, 235, 200), 0.18f), Color::mix(accent, Color(120, 20, 0), 0.35f));
    g.rrectLine(x, y, w, h, 6, 1.5f, Color(255, 220, 180, 120));
  } else {
    g.rrect(x, y, w, h, 6, theme::panel);
    g.rrectLine(x, y, w, h, 6, 1.5f, theme::line);
  }
  TextStyle ts;
  ts.font = Font::Head;
  ts.bold = true;
  ts.size = h * 0.40f;
  ts.tracking = 4;
  ts.align = Align::Center;
  ts.color = enabled ? Color(20, 10, 6) : theme::faint;
  Vec2 m = g.measure(label, ts);
  g.lintMute(1);
  g.text(label, x + w / 2, y + (h - m.y) / 2, ts);
  g.lintMute(-1);
  if (g.lintOn() && m.x > w - 24) g.lintFail("button '" + label + "' is wider than its button (" + std::to_string(static_cast<int>(m.x)) + " > " + std::to_string(static_cast<int>(w)) + ")");
  g.lintBox(Gfx::Box::Button, x, y, w, h, label);
}

void placeholder(Gfx& g, float x, float y, float w, float h, const std::string& name, float alpha) {
  Color t = tint(name);
  g.rrectGrad(x, y, w, h, 10, Color::mix(theme::panel, t, 0.14f).alpha(alpha), Color::mix(theme::panel, t, 0.04f).alpha(alpha));
  g.stripes(x + 6, y + 6, w - 12, h - 12, t.alpha(0.13f * alpha), 16, 2.5f);
  std::string initials;
  bool take = true;
  for (unsigned char c : name) {
    if (c == ' ' || c == '-') take = true;
    else if (take && std::isalnum(c)) { initials += static_cast<char>(std::toupper(c)); take = false; }
    if (initials.size() >= 2) break;
  }
  if (initials.empty()) initials = "?";
  TextStyle ts;
  ts.font = Font::Head;
  ts.bold = true;
  ts.size = h * 0.34f;
  ts.tracking = 4;
  ts.align = Align::Center;
  ts.color = t.alpha(0.85f * alpha);
  Vec2 m = g.measure(initials, ts);
  g.text(initials, x + w / 2, y + (h - m.y) / 2, ts);
}

}  // namespace vos::ui
