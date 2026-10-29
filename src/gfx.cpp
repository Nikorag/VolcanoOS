#include "gfx.hpp"

#include <algorithm>
#include <cmath>

#include "util.hpp"

namespace vos {

namespace {

constexpr float kPi = 3.14159265f;

// Perimeter of a rounded rect, clockwise from the top-left arc's start.
void perimeter(float x, float y, float w, float h, float r, int seg, std::vector<SDL_FPoint>& out) {
  r = std::max(0.f, std::min(r, std::min(w, h) * 0.5f));
  if (r < 0.5f) seg = 1;
  const struct { float cx, cy, a0; } corners[4] = {
      {x + r, y + r, 180.f}, {x + w - r, y + r, 270.f}, {x + w - r, y + h - r, 0.f}, {x + r, y + h - r, 90.f}};
  for (const auto& c : corners) {
    for (int i = 0; i <= seg; ++i) {
      float a = (c.a0 + 90.f * i / seg) * kPi / 180.f;
      out.push_back({c.cx + std::cos(a) * r, c.cy + std::sin(a) * r});
    }
  }
}

uint32_t nextCodepoint(const std::string& s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  uint32_t cp;
  int n;
  if (c < 0x80) { cp = c; n = 1; }
  else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
  else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
  else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
  else { cp = 0xFFFD; n = 1; }
  for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
  i += n;
  return cp;
}

std::string encodeUtf8(uint32_t cp) {
  std::string o;
  if (cp < 0x80) o += static_cast<char>(cp);
  else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3f)); }
  else if (cp < 0x10000) {
    o += static_cast<char>(0xE0 | (cp >> 12));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    o += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    o += static_cast<char>(0xF0 | (cp >> 18));
    o += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    o += static_cast<char>(0x80 | (cp & 0x3f));
  }
  return o;
}

SDL_Color sc(Color c) { return SDL_Color{c.r, c.g, c.b, c.a}; }

}  // namespace

bool Gfx::init(SDL_Window* w, SDL_Renderer* r) {
  win_ = w;
  r_ = r;
  monoPath_ = (assetDir() / "fonts" / "JetBrainsMono.ttf").string();
  headPath_ = (assetDir() / "fonts" / "Archivo.ttf").string();
  SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);

  // One soft radial falloff reused for every glow, ember and halo.
  const int N = 128;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, N, N, 32, SDL_PIXELFORMAT_RGBA32);
  if (!s) return false;
  auto* px = static_cast<uint8_t*>(s->pixels);
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
      float d = std::sqrt(dx * dx + dy * dy);
      float a = d >= 1 ? 0 : std::pow(1 - d, 2.2f);
      uint8_t* p = px + y * s->pitch + x * 4;
      p[0] = p[1] = p[2] = 255;
      p[3] = static_cast<uint8_t>(a * 255);
    }
  }
  glowTex_ = SDL_CreateTextureFromSurface(r_, s);
  SDL_FreeSurface(s);
  if (!glowTex_) return false;
  SDL_SetTextureBlendMode(glowTex_, SDL_BLENDMODE_ADD);
  return true;
}

void Gfx::shutdown() {
  for (auto& [k, e] : strings_) if (e.tex) SDL_DestroyTexture(e.tex);
  for (auto& [k, g] : glyphs_) if (g.tex) SDL_DestroyTexture(g.tex);
  for (auto& [k, f] : fonts_) TTF_CloseFont(f);
  strings_.clear();
  glyphs_.clear();
  fonts_.clear();
  if (glowTex_) SDL_DestroyTexture(glowTex_);
  glowTex_ = nullptr;
}

void Gfx::begin(Color clear) {
  int ow = 0, oh = 0;
  SDL_GetRendererOutputSize(r_, &ow, &oh);
  if (oh < 1) oh = 1;
  scale_ = oh / 1080.f;
  w_ = ow / scale_;
  SDL_RenderSetScale(r_, scale_, scale_);
  SDL_RenderSetClipRect(r_, nullptr);
  SDL_SetRenderDrawColor(r_, clear.r, clear.g, clear.b, clear.a);
  SDL_RenderClear(r_);
  ++frame_;
  if (frame_ % 240 == 0) evict();
}

void Gfx::present() { SDL_RenderPresent(r_); }

void Gfx::evict() {
  for (auto it = strings_.begin(); it != strings_.end();) {
    if (it->second.used + 600 < frame_) {
      if (it->second.tex) SDL_DestroyTexture(it->second.tex);
      it = strings_.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- geometry ---------------------------------------------------------------

void Gfx::tris(const std::vector<SDL_Vertex>& v, const std::vector<int>& idx) {
  if (v.empty() || idx.empty()) return;
  SDL_RenderGeometry(r_, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

void Gfx::rect(float x, float y, float w, float h, Color c) {
  if (c.a == 0) return;
  SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
  SDL_FRect fr{x, y, w, h};
  SDL_RenderFillRectF(r_, &fr);
}

void Gfx::quadGrad(float x, float y, float w, float h, Color tl, Color tr, Color br, Color bl) {
  SDL_Vertex v[4] = {{{x, y}, sc(tl), {0, 0}}, {{x + w, y}, sc(tr), {0, 0}}, {{x + w, y + h}, sc(br), {0, 0}},
                     {{x, y + h}, sc(bl), {0, 0}}};
  int idx[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

void Gfx::rrect(float x, float y, float w, float h, float r, Color c) {
  rrectGrad(x, y, w, h, r, c, c);
}

void Gfx::rrectGrad(float x, float y, float w, float h, float r, Color top, Color bottom) {
  if (top.a == 0 && bottom.a == 0) return;
  thread_local std::vector<SDL_FPoint> p;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  p.clear(); v.clear(); idx.clear();
  perimeter(x, y, w, h, r, 6, p);
  v.push_back({{x + w / 2, y + h / 2}, sc(Color::mix(top, bottom, 0.5f)), {0, 0}});
  for (auto& pt : p) v.push_back({pt, sc(Color::mix(top, bottom, h > 0 ? (pt.y - y) / h : 0)), {0, 0}});
  int n = static_cast<int>(p.size());
  for (int i = 0; i < n; ++i) {
    idx.push_back(0);
    idx.push_back(1 + i);
    idx.push_back(1 + (i + 1) % n);
  }
  tris(v, idx);
}

void Gfx::rrectLine(float x, float y, float w, float h, float r, float thick, Color c) {
  if (c.a == 0) return;
  thread_local std::vector<SDL_FPoint> outer, inner;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  outer.clear(); inner.clear(); v.clear(); idx.clear();
  perimeter(x, y, w, h, r, 6, outer);
  perimeter(x + thick, y + thick, w - thick * 2, h - thick * 2, std::max(0.f, r - thick), 6, inner);
  if (outer.size() != inner.size()) return;
  for (auto& p : outer) v.push_back({p, sc(c), {0, 0}});
  for (auto& p : inner) v.push_back({p, sc(c), {0, 0}});
  int n = static_cast<int>(outer.size());
  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;
    idx.insert(idx.end(), {i, j, n + i, j, n + j, n + i});
  }
  tris(v, idx);
}

void Gfx::circle(float cx, float cy, float r, Color c) {
  if (c.a == 0 || r <= 0) return;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  v.clear(); idx.clear();
  int seg = std::max(16, std::min(72, static_cast<int>(r * scale_ * 0.6f)));
  v.push_back({{cx, cy}, sc(c), {0, 0}});
  for (int i = 0; i < seg; ++i) {
    float a = 2 * kPi * i / seg;
    v.push_back({{cx + std::cos(a) * r, cy + std::sin(a) * r}, sc(c), {0, 0}});
    idx.insert(idx.end(), {0, 1 + i, 1 + (i + 1) % seg});
  }
  tris(v, idx);
}

void Gfx::arc(float cx, float cy, float r, float thick, float a0, float a1, Color c) {
  if (c.a == 0 || a1 <= a0) return;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  v.clear(); idx.clear();
  int seg = std::max(8, static_cast<int>((a1 - a0) / (2 * kPi) * 72));
  for (int i = 0; i <= seg; ++i) {
    float a = a0 + (a1 - a0) * i / seg;
    v.push_back({{cx + std::cos(a) * r, cy + std::sin(a) * r}, sc(c), {0, 0}});
    v.push_back({{cx + std::cos(a) * (r - thick), cy + std::sin(a) * (r - thick)}, sc(c), {0, 0}});
    if (i > 0) {
      int k = i * 2;
      idx.insert(idx.end(), {k - 2, k - 1, k, k - 1, k + 1, k});
    }
  }
  tris(v, idx);
}

void Gfx::line(float x1, float y1, float x2, float y2, float thick, Color c) {
  if (c.a == 0) return;
  float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f) return;
  float nx = -dy / len * thick / 2, ny = dx / len * thick / 2;
  SDL_Vertex v[4] = {{{x1 + nx, y1 + ny}, sc(c), {0, 0}}, {{x2 + nx, y2 + ny}, sc(c), {0, 0}},
                     {{x2 - nx, y2 - ny}, sc(c), {0, 0}}, {{x1 - nx, y1 - ny}, sc(c), {0, 0}}};
  int idx[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

void Gfx::polygon(const std::vector<SDL_FPoint>& pts, Color c) {
  if (pts.size() < 3 || c.a == 0) return;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  v.clear(); idx.clear();
  for (auto& p : pts) v.push_back({p, sc(c), {0, 0}});
  for (size_t i = 1; i + 1 < pts.size(); ++i) idx.insert(idx.end(), {0, static_cast<int>(i), static_cast<int>(i + 1)});
  tris(v, idx);
}

void Gfx::polyStrip(const std::vector<SDL_FPoint>& top, float bottomY, Color topC, Color bottomC) {
  if (top.size() < 2) return;
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  v.clear(); idx.clear();
  for (auto& p : top) {
    v.push_back({p, sc(topC), {0, 0}});
    v.push_back({{p.x, bottomY}, sc(bottomC), {0, 0}});
  }
  for (size_t i = 0; i + 1 < top.size(); ++i) {
    int k = static_cast<int>(i) * 2;
    idx.insert(idx.end(), {k, k + 1, k + 2, k + 1, k + 3, k + 2});
  }
  tris(v, idx);
}

void Gfx::glow(float cx, float cy, float rx, float ry, Color c) {
  if (c.a == 0) return;
  SDL_SetTextureColorMod(glowTex_, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(glowTex_, c.a);
  SDL_FRect d{cx - rx, cy - ry, rx * 2, ry * 2};
  SDL_RenderCopyF(r_, glowTex_, nullptr, &d);
}

void Gfx::stripes(float x, float y, float w, float h, Color c, float step, float thick) {
  // Diagonal hatching clipped analytically to the rect (no clip state needed).
  for (float k = -h + step * 0.5f; k < w; k += step) {
    float u0 = std::max(0.f, k), u1 = std::min(w, k + h);
    if (u1 <= u0) continue;
    line(x + u0, y + (u0 - k), x + u1, y + (u1 - k), thick, c);
  }
}

void Gfx::clip(float x, float y, float w, float h) {
  SDL_Rect rc{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::ceil(w)),
              static_cast<int>(std::ceil(h))};
  SDL_RenderSetClipRect(r_, &rc);
}

void Gfx::unclip() { SDL_RenderSetClipRect(r_, nullptr); }

// ---- images -----------------------------------------------------------------

SDL_Texture* Gfx::makeTexture(SDL_Surface* s) {
  SDL_Texture* t = SDL_CreateTextureFromSurface(r_, s);
  if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}

void Gfx::image(SDL_Texture* t, float x, float y, float w, float h, float alpha) {
  if (!t || alpha <= 0) return;
  SDL_SetTextureColorMod(t, 255, 255, 255);
  SDL_SetTextureAlphaMod(t, static_cast<Uint8>(std::min(1.f, alpha) * 255));
  SDL_FRect d{x, y, w, h};
  SDL_RenderCopyF(r_, t, nullptr, &d);
}

void Gfx::cover(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha, float focusY) {
  if (!t || tw <= 0 || th <= 0 || alpha <= 0) return;
  float s = std::max(w / tw, h / th);
  float sw = w / s, sh = h / s;  // source window in texels
  SDL_Rect src{static_cast<int>((tw - sw) / 2), static_cast<int>((th - sh) * focusY), static_cast<int>(sw),
               static_cast<int>(sh)};
  SDL_SetTextureColorMod(t, 255, 255, 255);
  SDL_SetTextureAlphaMod(t, static_cast<Uint8>(std::min(1.f, alpha) * 255));
  SDL_FRect d{x, y, w, h};
  SDL_RenderCopyF(r_, t, &src, &d);
}

void Gfx::contain(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha) {
  if (!t || tw <= 0 || th <= 0) return;
  float s = std::min(w / tw, h / th);
  float dw = tw * s, dh = th * s;
  image(t, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, alpha);
}

void Gfx::imageFeather(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha, float fl,
                       float fr, float ft, float fb) {
  if (!t || tw <= 0 || th <= 0 || alpha <= 0) return;
  // Cover crop in normalised texture space.
  float s = std::max(w / tw, h / th);
  float u0 = (1 - (w / s) / tw) / 2, u1 = 1 - u0, v0 = (1 - (h / s) / th) / 2, v1 = 1 - v0;
  const int NX = 18, NY = 12;
  auto soft = [](float k) { k = std::min(1.f, std::max(0.f, k)); return k * k * (3 - 2 * k); };
  thread_local std::vector<SDL_Vertex> v;
  thread_local std::vector<int> idx;
  v.clear(); idx.clear();
  for (int j = 0; j <= NY; ++j) {
    for (int i = 0; i <= NX; ++i) {
      float fx = i / float(NX), fy = j / float(NY);
      float a = alpha;
      if (fl > 0) a *= soft(fx / fl);
      if (fr > 0) a *= soft((1 - fx) / fr);
      if (ft > 0) a *= soft(fy / ft);
      if (fb > 0) a *= soft((1 - fy) / fb);
      SDL_Vertex vx;
      vx.position = {x + fx * w, y + fy * h};
      vx.color = SDL_Color{255, 255, 255, static_cast<Uint8>(std::min(1.f, a) * 255)};
      vx.tex_coord = {u0 + (u1 - u0) * fx, v0 + (v1 - v0) * fy};
      v.push_back(vx);
    }
  }
  for (int j = 0; j < NY; ++j)
    for (int i = 0; i < NX; ++i) {
      int a = j * (NX + 1) + i, b = a + 1, c = a + NX + 1, d = c + 1;
      idx.insert(idx.end(), {a, b, c, b, d, c});
    }
  SDL_SetTextureColorMod(t, 255, 255, 255);
  SDL_SetTextureAlphaMod(t, 255);
  SDL_RenderGeometry(r_, t, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

// ---- text -------------------------------------------------------------------

TTF_Font* Gfx::font(const TextStyle& st) {
  int px = std::max(6, static_cast<int>(std::lround(st.size * scale_)));
  FontKey k{static_cast<int>(st.font), px, st.bold ? 1 : 0};
  auto it = fonts_.find(k);
  if (it != fonts_.end()) return it->second;
  TTF_Font* f = TTF_OpenFont((st.font == Font::Mono ? monoPath_ : headPath_).c_str(), px);
  if (!f) {
    logf("font load failed: %s", TTF_GetError());
    fonts_[k] = nullptr;
    return nullptr;
  }
  TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
  if (st.bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
  fonts_[k] = f;
  if (!fontIds_.count(k)) fontIds_[k] = static_cast<int>(fontIds_.size());
  return f;
}

Gfx::TexEntry* Gfx::stringTex(TTF_Font* f, const FontKey& fk, const std::string& s, int wrapPx) {
  std::string key = std::to_string(fk.kind) + "|" + std::to_string(fk.px) + "|" + std::to_string(fk.bold) + "|" +
                    std::to_string(wrapPx) + "|" + s;
  auto it = strings_.find(key);
  if (it != strings_.end()) {
    it->second.used = frame_;
    return &it->second;
  }
  TexEntry e;
  SDL_Color white{255, 255, 255, 255};
  SDL_Surface* surf = wrapPx > 0 ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), white, static_cast<Uint32>(wrapPx))
                                 : TTF_RenderUTF8_Blended(f, s.c_str(), white);
  if (surf) {
    e.tex = SDL_CreateTextureFromSurface(r_, surf);
    e.w = surf->w;
    e.h = surf->h;
    SDL_FreeSurface(surf);
    if (e.tex) SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
  }
  e.used = frame_;
  return &(strings_[key] = e);
}

Gfx::Glyph* Gfx::glyph(TTF_Font* f, const FontKey& fk, uint32_t cp) {
  std::pair<int, uint32_t> key{fontIds_[fk], cp};
  auto it = glyphs_.find(key);
  if (it != glyphs_.end()) return &it->second;
  Glyph g;
  int minx, maxx, miny, maxy, adv = 0;
  if (TTF_GlyphMetrics32(f, cp, &minx, &maxx, &miny, &maxy, &adv) != 0) adv = fk.px / 2;
  g.adv = adv;
  if (cp != ' ') {
    SDL_Surface* s = TTF_RenderUTF8_Blended(f, encodeUtf8(cp).c_str(), SDL_Color{255, 255, 255, 255});
    if (s) {
      g.tex = SDL_CreateTextureFromSurface(r_, s);
      g.w = s->w;
      g.h = s->h;
      SDL_FreeSurface(s);
      if (g.tex) SDL_SetTextureBlendMode(g.tex, SDL_BLENDMODE_BLEND);
    }
  }
  return &(glyphs_[key] = g);
}

Vec2 Gfx::measure(const std::string& s, const TextStyle& st) {
  if (s.empty()) return {};
  TTF_Font* f = font(st);
  if (!f) return {};
  int px = static_cast<int>(std::lround(st.size * scale_));
  if (st.wrap > 0) {
    FontKey fk{static_cast<int>(st.font), std::max(6, px), st.bold ? 1 : 0};
    TexEntry* e = stringTex(f, fk, s, static_cast<int>(st.wrap * scale_));
    return {e->w / scale_, e->h / scale_};
  }
  if (st.tracking == 0) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return {w / scale_, h / scale_};
  }
  FontKey fk{static_cast<int>(st.font), std::max(6, px), st.bold ? 1 : 0};
  float w = 0;
  size_t i = 0;
  int n = 0;
  while (i < s.size()) {
    uint32_t cp = nextCodepoint(s, i);
    w += glyph(f, fk, cp)->adv / scale_ + st.tracking;
    ++n;
  }
  if (n) w -= st.tracking;
  return {w, TTF_FontHeight(f) / scale_};
}

std::string Gfx::ellipsize(const std::string& s, const TextStyle& st, float maxW) {
  TextStyle t = st;
  t.wrap = 0;
  if (measure(s, t).x <= maxW) return s;
  std::string out = s;
  while (!out.empty()) {
    // drop one UTF-8 code point
    size_t i = out.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(out[i]) & 0xC0) == 0x80) --i;
    out.erase(i);
    std::string cand = out + "\xE2\x80\xA6";
    if (measure(cand, t).x <= maxW) return cand;
  }
  return "";
}

Vec2 Gfx::text(const std::string& s, float x, float y, const TextStyle& st) {
  if (s.empty() || st.color.a == 0) return {};
  TTF_Font* f = font(st);
  if (!f) return {};
  int px = std::max(6, static_cast<int>(std::lround(st.size * scale_)));
  FontKey fk{static_cast<int>(st.font), px, st.bold ? 1 : 0};

  if (st.tracking == 0 || st.wrap > 0) {
    if (st.wrap > 0) {
      TTF_SetFontWrappedAlign(f, st.align == Align::Center ? TTF_WRAPPED_ALIGN_CENTER
                                  : st.align == Align::Right ? TTF_WRAPPED_ALIGN_RIGHT
                                                             : TTF_WRAPPED_ALIGN_LEFT);
      TTF_SetFontLineSkip(f, static_cast<int>(std::lround(px * st.lineHeight * 1.15f)));
    }
    TexEntry* e = stringTex(f, fk, s, st.wrap > 0 ? static_cast<int>(st.wrap * scale_) : 0);
    if (!e->tex) return {};
    float w = e->w / scale_, h = e->h / scale_;
    float dx = st.align == Align::Center && st.wrap <= 0 ? x - w / 2 : st.align == Align::Right && st.wrap <= 0 ? x - w : x;
    if (st.wrap > 0) dx = st.align == Align::Center ? x - st.wrap / 2 : st.align == Align::Right ? x - st.wrap : x;
    SDL_SetTextureColorMod(e->tex, st.color.r, st.color.g, st.color.b);
    SDL_SetTextureAlphaMod(e->tex, st.color.a);
    SDL_FRect d{dx, y, w, h};
    SDL_RenderCopyF(r_, e->tex, nullptr, &d);
    return {w, h};
  }

  Vec2 m = measure(s, st);
  float cx = st.align == Align::Center ? x - m.x / 2 : st.align == Align::Right ? x - m.x : x;
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = nextCodepoint(s, i);
    Glyph* g = glyph(f, fk, cp);
    if (g->tex) {
      SDL_SetTextureColorMod(g->tex, st.color.r, st.color.g, st.color.b);
      SDL_SetTextureAlphaMod(g->tex, st.color.a);
      SDL_FRect d{cx, y, g->w / scale_, g->h / scale_};
      SDL_RenderCopyF(r_, g->tex, nullptr, &d);
    }
    cx += g->adv / scale_ + st.tracking;
  }
  return m;
}

}  // namespace vos
