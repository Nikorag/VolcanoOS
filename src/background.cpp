#include "background.hpp"

#include <cmath>

namespace vos {

namespace {

float hash1(int n) {
  uint32_t x = static_cast<uint32_t>(n) * 747796405u + 2891336453u;
  x = ((x >> ((x >> 28) + 4)) ^ x) * 277803737u;
  x = (x >> 22) ^ x;
  return (x & 0xFFFFFF) / float(0xFFFFFF);
}

// Smooth 1-D value noise in [0,1].
float noise(float x, int salt) {
  int i = static_cast<int>(std::floor(x));
  float f = x - i;
  float u = f * f * (3 - 2 * f);
  return hash1(i * 31 + salt) * (1 - u) + hash1((i + 1) * 31 + salt) * u;
}

float fbm(float x, int salt) {
  return noise(x, salt) * 0.6f + noise(x * 2.3f, salt + 7) * 0.28f + noise(x * 5.1f, salt + 13) * 0.12f;
}

}  // namespace

float Background::rnd() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return (seed_ >> 8) / float(1 << 24);
}

void Background::init() {
  embers_.assign(70, Ember{});
  ready_ = false;
}

void Background::spawn(Ember& e, float cx, float cy, float W, bool initial) {
  // Most embers leave the crater; some lift off the lava veins and the far ground.
  float roll = rnd();
  if (roll < 0.55f) {
    e.x = cx + (rnd() - 0.5f) * 150;
    e.y = cy + rnd() * 20;
  } else {
    e.x = cx + (rnd() - 0.5f) * 900;
    e.y = cy + 120 + rnd() * 420;
  }
  e.vx = (rnd() - 0.35f) * 26;
  e.vy = -(30 + rnd() * 90);
  e.max = 3.5f + rnd() * 6.5f;
  e.life = initial ? rnd() * e.max : 0;
  e.size = 5 + rnd() * 11;
  e.phase = rnd() * 6.28f;
  (void)W;
}

void Background::draw(Gfx& g, double t, float calm) {
  const float W = g.W(), H = g.H();
  float dt = last_ > 0 ? static_cast<float>(std::min(0.1, t - last_)) : 0.016f;
  last_ = t;

  // Slow breathing, with a faster flicker riding on top.
  float breathe = 0.78f + 0.22f * std::sin(static_cast<float>(t) * 0.7f);
  float flicker = breathe + 0.08f * std::sin(static_cast<float>(t) * 7.3f) + 0.05f * std::sin(static_cast<float>(t) * 13.1f);

  const float cx = W * 0.76f, craterY = H * 0.60f;

  // Sky: basalt black warming towards the horizon.
  g.vgrad(0, 0, W, H, Color(9, 7, 7), Color(30, 15, 9));

  // Heat bloom behind the cone.
  g.glow(cx, craterY + 40, 980, 640, Color(255, 90, 24, static_cast<uint8_t>(54 * flicker)));
  g.glow(cx, craterY, 420, 300, Color(255, 120, 40, static_cast<uint8_t>(78 * flicker)));

  // Far range.
  {
    std::vector<SDL_FPoint> p;
    for (float x = 0; x <= W + 24; x += 24) p.push_back({x, H * 0.80f - fbm(x * 0.004f, 3) * 150});
    g.polyStrip(p, H, Color(26, 17, 14), Color(14, 10, 9));
  }

  // The volcano: jagged flanks sweeping up to a notched crater.
  {
    std::vector<SDL_FPoint> p;
    const float baseHalf = 760, rim = 92;
    for (int i = 0; i <= 40; ++i) {
      float u = i / 40.f;                           // 0 = left foot, 1 = right foot
      float side = (u - 0.5f) * 2;                  // -1..1
      float a = std::fabs(side);
      float profile = std::pow(1 - a, 1.35f);       // concave flanks
      float y = H + 20 - profile * (H + 20 - craterY);
      float jag = (fbm(u * 9.f, 21) - 0.35f) * 46 * (0.25f + a);
      float x = cx + side * baseHalf;
      if (a < 0.14f) {  // crater lip
        float lip = (0.14f - a) / 0.14f;
        y = craterY + lip * lip * 34 + (side < 0 ? 0 : 5);
        x = cx + side * rim * (a / 0.14f + 0.2f) * (side < 0 ? 1.0f : 1.0f);
      }
      p.push_back({x, y + jag});
    }
    g.polyStrip(p, H + 30, Color(34, 21, 16), Color(13, 9, 8));

    // Rim light on the crown.
    for (size_t i = 0; i + 1 < p.size(); ++i) {
      float a = std::fabs((static_cast<float>(i) / (p.size() - 1) - 0.5f) * 2);
      if (a > 0.5f) continue;
      float k = (0.5f - a) * 2;
      g.line(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, 3.0f, Color(255, 130, 50, static_cast<uint8_t>(90 * k * flicker)));
    }

    // Lava veins running down the flanks (additive).
    struct Vein { float startSide, drift, len; int salt; };
    const Vein veins[] = {{-0.05f, -0.9f, 0.55f, 1}, {0.07f, 0.8f, 0.48f, 2}, {0.0f, 0.25f, 0.38f, 3}, {-0.1f, -0.35f, 0.3f, 4}};
    for (const auto& v : veins) {
      float x = cx + v.startSide * 220, y = craterY + 26;
      float len = (H - craterY) * v.len;
      int steps = 22;
      for (int s = 0; s < steps; ++s) {
        float f = s / float(steps);
        float nx = x + v.drift * (len / steps) * 0.9f + (noise(f * 6 + v.salt * 3, v.salt) - 0.5f) * 26;
        float ny = y + len / steps;
        float fade = (1 - f) * (0.6f + 0.4f * std::sin(static_cast<float>(t) * 1.4f + v.salt + f * 5));
        g.line(x, y, nx, ny, 4.0f * (1 - f * 0.6f), Color(255, 110, 36, static_cast<uint8_t>(150 * fade)));
        g.glow(nx, ny, 34, 34, Color(255, 100, 30, static_cast<uint8_t>(60 * fade)));
        x = nx;
        y = ny;
      }
    }
  }

  // Crater mouth.
  g.glow(cx, craterY + 6, 150, 46, Color(255, 190, 90, static_cast<uint8_t>(190 * flicker)));
  g.glow(cx, craterY - 4, 70, 24, Color(255, 240, 190, static_cast<uint8_t>(150 * flicker)));

  // Embers.
  for (auto& e : embers_) {
    if (!ready_ || e.life >= e.max) spawn(e, cx, craterY, W, !ready_);
    e.life += dt;
    e.y += e.vy * dt;
    e.x += (e.vx + std::sin(e.life * 1.7f + e.phase) * 18) * dt;
    float f = e.life / e.max;
    float a = std::sin(f * 3.14159f);  // fade in and out
    float tw = 0.7f + 0.3f * std::sin(e.life * 9 + e.phase);
    g.glow(e.x, e.y, e.size, e.size, Color(255, 150, 50, static_cast<uint8_t>(210 * a * tw)));
    g.glow(e.x, e.y, e.size * 0.35f, e.size * 0.35f, Color(255, 230, 170, static_cast<uint8_t>(255 * a * tw)));
  }
  ready_ = true;

  // Foreground ridge to seat the UI.
  {
    std::vector<SDL_FPoint> p;
    for (float x = 0; x <= W + 24; x += 24) p.push_back({x, H * 0.93f - fbm(x * 0.006f, 41) * 90});
    g.polyStrip(p, H, Color(10, 8, 7, 235), Color(6, 5, 5, 255));
  }

  // Keep foreground content legible.
  if (calm > 0) g.rect(0, 0, W, H, Color(8, 6, 6, static_cast<uint8_t>(255 * calm)));
}

}  // namespace vos
