#include "art.hpp"

#include <SDL_image.h>

#include <algorithm>

#include "http.hpp"
#include "sha256.hpp"

namespace vos {

namespace {

fs::path cachePath(const std::string& url) {
  auto d = Sha256::of(url);
  return dataDir() / "art" / (Sha256::hex(d).substr(0, 32) + ".img");
}

using SurfacePtr = std::shared_ptr<SDL_Surface>;

// Area-average downscale (SDL_BlitScaled is nearest-neighbour and sparkles on halftone art).
SurfacePtr boxShrink(SurfacePtr in, int maxDim) {
  if (!in || std::max(in->w, in->h) <= maxDim) return in;
  SDL_Surface* src = SDL_ConvertSurfaceFormat(in.get(), SDL_PIXELFORMAT_RGBA32, 0);
  if (!src) return in;
  const float k = static_cast<float>(maxDim) / std::max(src->w, src->h);
  const int dw = std::max(1, int(src->w * k)), dh = std::max(1, int(src->h * k));
  SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_RGBA32);
  if (!dst) {
    SDL_FreeSurface(src);
    return in;
  }
  const auto* sp = static_cast<const uint8_t*>(src->pixels);
  auto* dp = static_cast<uint8_t*>(dst->pixels);
  for (int y = 0; y < dh; ++y) {
    const int y0 = y * src->h / dh, y1 = std::max(y0 + 1, (y + 1) * src->h / dh);
    for (int x = 0; x < dw; ++x) {
      const int x0 = x * src->w / dw, x1 = std::max(x0 + 1, (x + 1) * src->w / dw);
      unsigned acc[4] = {0, 0, 0, 0};
      for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* row = sp + yy * src->pitch + x0 * 4;
        for (int xx = x0; xx < x1; ++xx, row += 4)
          for (int c = 0; c < 4; ++c) acc[c] += row[c];
      }
      const unsigned n = static_cast<unsigned>((y1 - y0) * (x1 - x0));
      uint8_t* o = dp + y * dst->pitch + x * 4;
      for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(acc[c] / n);
    }
  }
  SDL_FreeSurface(src);
  return SurfacePtr(dst, SDL_FreeSurface);
}

SurfacePtr decode(const std::string& bytes) {
  SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
  if (!rw) return nullptr;
  SDL_Surface* s = IMG_Load_RW(rw, 1);
  if (!s) return nullptr;
  // Keep GPU memory sane: 1920 on the long edge is plenty for a 1080p UI.
  const int kMax = 1920;
  if (s->w > kMax || s->h > kMax) {
    float k = static_cast<float>(kMax) / std::max(s->w, s->h);
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, int(s->w * k)), std::max(1, int(s->h * k)), 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    if (d) {
      SDL_BlitScaled(s, nullptr, d, nullptr);
      SDL_FreeSurface(s);
      s = d;
    }
  }
  return SurfacePtr(s, SDL_FreeSurface);
}

}  // namespace

Art::~Art() {
  for (auto& [url, img] : images_)
    if (img.tex) SDL_DestroyTexture(img.tex);
}

const ArtImage* Art::get(const std::string& url, int maxDim) {
  if (url.empty()) return nullptr;
  const std::string key = maxDim > 0 ? url + "#" + std::to_string(maxDim) : url;
  auto it = images_.find(key);
  if (it != images_.end()) return it->second.state == ArtImage::Ready ? &it->second : nullptr;

  images_[key] = ArtImage{};
  pool_.run<SurfacePtr>(
      main_,
      [url, maxDim]() -> SurfacePtr {
        fs::path p = cachePath(url);
        std::string bytes;
        if (url.rfind("file://", 0) == 0) {  // bundled artwork: read in place, never cached
          if (!readFile(url.substr(7), bytes) || bytes.empty()) return nullptr;
          return boxShrink(decode(bytes), maxDim);
        }
        if (!readFile(p, bytes) || bytes.empty()) {
          HttpRequest rq;
          rq.url = url;
          rq.timeoutSec = 30;
          HttpResponse r = httpRequest(rq);
          if (!r.ok() || r.body.empty()) {
            logf("artwork fetch failed (%ld): %s", r.status, url.c_str());
            return nullptr;
          }
          bytes = std::move(r.body);
          writeFileAtomic(p, bytes);
        }
        SurfacePtr s = decode(bytes);
        if (!s) {
          logf("artwork decode failed: %s", url.c_str());
          std::error_code ec;
          fs::remove(p, ec);  // don't keep a poisoned cache entry
        }
        return boxShrink(s, maxDim);
      },
      [this, key](SurfacePtr s) {
        auto it = images_.find(key);
        if (it == images_.end()) return;
        if (!s) {
          it->second.state = ArtImage::Failed;
          return;
        }
        it->second.tex = gfx_.makeTexture(s.get());
        it->second.w = s->w;
        it->second.h = s->h;
        it->second.readyAt = nowSeconds();
        it->second.state = it->second.tex ? ArtImage::Ready : ArtImage::Failed;
      });
  return nullptr;
}

float Art::fade(const ArtImage* a, double now, double dur) const {
  if (!a) return 0;
  return static_cast<float>(std::min(1.0, std::max(0.0, (now - a->readyAt) / dur)));
}

}  // namespace vos
