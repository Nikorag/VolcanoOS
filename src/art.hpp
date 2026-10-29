// Game artwork: fetched once, cached on disk, uploaded to the GPU lazily.
// get() never blocks — it returns nullptr until the image is ready.
#pragma once

#include <SDL.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "gfx.hpp"
#include "util.hpp"

namespace vos {

struct ArtImage {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0;
  double readyAt = 0;  // for fade-in
  enum State { Loading, Ready, Failed } state = Loading;
};

class Art {
 public:
  Art(Gfx& gfx, Pool& pool, MainQueue& main) : gfx_(gfx), pool_(pool), main_(main) {}
  ~Art();

  // nullptr until ready. `url` may be empty (returns nullptr).
  const ArtImage* get(const std::string& url);
  // 0..1 fade-in progress once ready.
  float fade(const ArtImage* a, double now, double dur = 0.45) const;

 private:
  Gfx& gfx_;
  Pool& pool_;
  MainQueue& main_;
  std::unordered_map<std::string, ArtImage> images_;
};

}  // namespace vos
