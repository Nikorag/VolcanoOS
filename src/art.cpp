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

const ArtImage* Art::get(const std::string& url) {
  if (url.empty()) return nullptr;
  auto it = images_.find(url);
  if (it != images_.end()) return it->second.state == ArtImage::Ready ? &it->second : nullptr;

  images_[url] = ArtImage{};
  pool_.run<SurfacePtr>(
      main_,
      [url]() -> SurfacePtr {
        fs::path p = cachePath(url);
        std::string bytes;
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
        return s;
      },
      [this, url](SurfacePtr s) {
        auto it = images_.find(url);
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
