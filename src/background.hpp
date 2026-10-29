// The living backdrop: a basalt ridge-line, a glowing crater with lava veins,
// and embers drifting up. Pure vector drawing — no assets.
#pragma once

#include <vector>

#include "gfx.hpp"

namespace vos {

class Background {
 public:
  void init();
  // `calm` 0..1 darkens the scene so content on top stays readable.
  void draw(Gfx& g, double t, float calm);

 private:
  struct Ember {
    float x, y, vx, vy, life, max, size, phase;
  };
  void spawn(Ember& e, float cx, float cy, float W, bool initial);
  std::vector<Ember> embers_;
  double last_ = 0;
  uint32_t seed_ = 1234567;
  float rnd();
  bool ready_ = false;
};

}  // namespace vos
