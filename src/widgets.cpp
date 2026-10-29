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
    ts.tracking = 1;
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
  ts.tracking = 1;
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

float promptWidth(Gfx& g, PadStyle style, Action a, const std::string& label, float size) {
  float w = glyph(g, style, a, 0, 0, size, 1, false);
  if (label.empty()) return w;
  TextStyle ts;
  ts.size = size * 0.78f;
  ts.tracking = 2.4f;
  return w + size * 0.5f + g.measure(label, ts).x;
}

float prompt(Gfx& g, PadStyle style, Action a, const std::string& label, float x, float y, float size, float alpha) {
  float w = glyph(g, style, a, x, y, size, alpha, true);
  if (label.empty()) return w;
  TextStyle ts;
  ts.size = size * 0.78f;
  ts.tracking = 2.4f;
  ts.color = theme::dim.alpha(alpha);
  float gh = size * 1.15f;
  Vec2 m = g.measure(label, ts);
  g.text(label, x + w + size * 0.5f, y + (gh - m.y) / 2, ts);
  return w + size * 0.5f + m.x;
}

float chip(Gfx& g, const std::string& text, float x, float y, Color fg, Color border, float size, Color fill) {
  TextStyle ts;
  ts.size = size;
  ts.tracking = 2.2f;
  ts.color = fg;
  Vec2 m = g.measure(text, ts);
  float w = m.x + size * 1.3f, h = size * 1.9f;
  if (fill.a) g.rrect(x, y, w, h, 3, fill);
  g.rrectLine(x, y, w, h, 3, 1.4f, border);
  g.text(text, x + size * 0.65f, y + (h - m.y) / 2, ts);
  return w;
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
  g.text(label, x + w / 2, y + (h - m.y) / 2, ts);
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
