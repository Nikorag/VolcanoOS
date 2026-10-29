// 2D drawing toolkit over SDL_Renderer: everything is laid out in "UI units"
// (1080 units tall, width follows the window's aspect) and rasterised at the
// real pixel density, so text stays sharp on high-DPI and 4K screens.
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <map>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

namespace vos {

struct Color {
  uint8_t r = 255, g = 255, b = 255, a = 255;
  constexpr Color() = default;
  constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
  Color alpha(float k) const {
    float v = a * k;
    return Color(r, g, b, static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v));
  }
  static Color mix(const Color& x, const Color& y, float t) {
    auto l = [t](int p, int q) { return static_cast<uint8_t>(p + (q - p) * (t < 0 ? 0 : t > 1 ? 1 : t)); };
    return Color(l(x.r, y.r), l(x.g, y.g), l(x.b, y.b), l(x.a, y.a));
  }
};

// Volcano OS palette: basalt blacks, magma oranges, with Zonto's signal green
// kept for "online / installed / ready".
namespace theme {
constexpr Color bg{11, 9, 8};
constexpr Color panel{24, 19, 17};
constexpr Color panelHi{38, 30, 26};
constexpr Color line{64, 49, 41};
constexpr Color text{245, 237, 229};
constexpr Color dim{160, 142, 129};
constexpr Color faint{132, 117, 105};
constexpr Color magma{255, 106, 31};
constexpr Color ember{255, 178, 71};
constexpr Color lava{226, 52, 28};
constexpr Color ok{61, 232, 140};  // Zonto signal green
constexpr Color warn{255, 204, 77};
constexpr Color err{255, 92, 79};
}  // namespace theme

enum class Font { Mono, Head };
enum class Align { Left, Center, Right };

struct TextStyle {
  Font font = Font::Mono;
  float size = 20;       // UI units
  Color color = theme::text;
  float tracking = 0;    // extra letter spacing, UI units
  bool bold = false;
  Align align = Align::Left;
  float wrap = 0;        // >0: wrap to this width (untracked text only)
  float lineHeight = 1.3f;
};

struct Vec2 {
  float x = 0, y = 0;
};

class Gfx {
 public:
  bool init(SDL_Window* w, SDL_Renderer* r);
  void shutdown();

  // Start a frame: derives the UI scale from the output size and clears.
  void begin(Color clear = theme::bg);
  void present();

  SDL_Renderer* renderer() const { return r_; }
  float W() const { return w_; }
  float H() const { return 1080.f; }
  float scale() const { return scale_; }

  // ---- shapes -------------------------------------------------------------
  void rect(float x, float y, float w, float h, Color c);
  void rrect(float x, float y, float w, float h, float r, Color c);
  void rrectGrad(float x, float y, float w, float h, float r, Color top, Color bottom);
  void rrectLine(float x, float y, float w, float h, float r, float thick, Color c);
  void quadGrad(float x, float y, float w, float h, Color tl, Color tr, Color br, Color bl);
  void vgrad(float x, float y, float w, float h, Color top, Color bottom) { quadGrad(x, y, w, h, top, top, bottom, bottom); }
  void hgrad(float x, float y, float w, float h, Color left, Color right) { quadGrad(x, y, w, h, left, right, right, left); }
  void circle(float cx, float cy, float r, Color c);
  void arc(float cx, float cy, float r, float thick, float a0, float a1, Color c);  // radians, 0 = 3 o'clock
  void ring(float cx, float cy, float r, float thick, Color c) { arc(cx, cy, r, thick, 0, 6.2831853f, c); }
  void line(float x1, float y1, float x2, float y2, float thick, Color c);
  void polygon(const std::vector<SDL_FPoint>& pts, Color c);  // convex or fan-from-first
  void polyStrip(const std::vector<SDL_FPoint>& top, float bottomY, Color topC, Color bottomC);  // fill from a curve to y
  void glow(float cx, float cy, float rx, float ry, Color c);  // additive radial falloff
  void stripes(float x, float y, float w, float h, Color c, float step = 14, float thick = 2);
  void clip(float x, float y, float w, float h);
  void unclip();

  // ---- images ---------------------------------------------------------------
  void image(SDL_Texture* t, float x, float y, float w, float h, float alpha = 1.f);
  void cover(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha = 1.f, float focusY = 0.5f);
  void contain(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha = 1.f);
  // Cover-fit image whose edges dissolve into whatever is behind it. Feathers
  // are fractions of the destination size (0 = hard edge).
  void imageFeather(SDL_Texture* t, int tw, int th, float x, float y, float w, float h, float alpha,
                    float fl, float fr, float ft, float fb);

  // ---- text -------------------------------------------------------------------
  Vec2 text(const std::string& s, float x, float y, const TextStyle& st);
  Vec2 measure(const std::string& s, const TextStyle& st);
  std::string ellipsize(const std::string& s, const TextStyle& st, float maxW);

  SDL_Texture* makeTexture(SDL_Surface* s);  // takes no ownership of the surface

  // The Zonto Volcano mark (assets/logo/zonto-volcano.svg), rasterised crisply at
  // the size it's drawn. Centred on (cx, cy), `h` UI units tall. Returns its width.
  float logo(float cx, float cy, float h, float alpha = 1.f);

 private:
  struct FontKey {
    int kind, px, bold;
    bool operator<(const FontKey& o) const {
      return std::tie(kind, px, bold) < std::tie(o.kind, o.px, o.bold);
    }
  };
  struct TexEntry {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    uint64_t used = 0;
  };
  struct Glyph {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, adv = 0;
  };
  TTF_Font* font(const TextStyle& st);
  TexEntry* stringTex(TTF_Font* f, const FontKey& fk, const std::string& s, int wrapPx);
  Glyph* glyph(TTF_Font* f, const FontKey& fk, uint32_t cp);
  void evict();
  void tris(const std::vector<SDL_Vertex>& v, const std::vector<int>& idx);

  SDL_Window* win_ = nullptr;
  SDL_Renderer* r_ = nullptr;
  float w_ = 1920, scale_ = 1;
  uint64_t frame_ = 0;
  SDL_Texture* glowTex_ = nullptr;
  std::map<FontKey, TTF_Font*> fonts_;
  std::unordered_map<std::string, TexEntry> strings_;
  std::map<std::pair<int, uint32_t>, Glyph> glyphs_;  // key: (font id, codepoint)
  std::map<FontKey, int> fontIds_;
  struct LogoTex { SDL_Texture* tex = nullptr; int w = 0, h = 0; };
  std::map<int, LogoTex> logos_;  // by pixel height
  bool logoMissing_ = false;
  std::string monoPath_, headPath_;
};

}  // namespace vos
