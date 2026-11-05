// Linux overlay: a second, fullscreen SDL window that exists only while the in-game menu is open.
//
// Under the console's kiosk compositor (cage) a client cannot raise itself above another client, but a
// window that is newly mapped lands on top and takes keyboard focus; destroying it hands focus back to
// the game. So the "overlay" is created on demand and torn down when the menu closes. It is opt-in
// (VOS_OVERLAY_WINDOW=1, set by the console session): other Linux setups, like the Steam Deck's
// gamescope, keep the older "draw the menu in the launcher window" behaviour.
#ifdef __linux__

#include <SDL.h>

#include <cstdlib>

#include "overlay.hpp"
#include "util.hpp"

namespace vos {

namespace {

class SdlOverlay : public OverlayWindow {
 public:
  ~SdlOverlay() override { destroy(); }

  void present(const void* pixels, int w, int h, int pitch) override {
    if (!win_ && !create()) return;
    if (!tex_ || w != tw_ || h != th_) {
      if (tex_) SDL_DestroyTexture(tex_);
      tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
      if (tex_) SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND);
      tw_ = w;
      th_ = h;
    }
    if (!tex_) return;
    SDL_UpdateTexture(tex_, nullptr, pixels, pitch);
    SDL_SetRenderDrawColor(ren_, 11, 9, 8, 255);  // theme::bg: the menu is drawn over a dark backdrop
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    SDL_RenderPresent(ren_);
  }

  void show(bool visible) override {
    if (!visible) destroy();
  }
  bool visible() const override { return win_ != nullptr; }
  bool takesFocus() const override { return true; }
  uint32_t windowId() const override { return win_ ? SDL_GetWindowID(win_) : 0; }

 private:
  bool create() {
    win_ = SDL_CreateWindow("Volcano OS menu", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
    if (!win_) {
      logf("overlay window: %s", SDL_GetError());
      return false;
    }
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
    if (!ren_) {
      logf("overlay renderer: %s", SDL_GetError());
      destroy();
      return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_ShowCursor(SDL_DISABLE);
    return true;
  }
  void destroy() {
    if (tex_) SDL_DestroyTexture(tex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    tex_ = nullptr;
    ren_ = nullptr;
    win_ = nullptr;
    tw_ = th_ = 0;
  }

  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture* tex_ = nullptr;
  int tw_ = 0, th_ = 0;
};

}  // namespace

std::unique_ptr<OverlayWindow> createOverlayWindow() {
  const char* on = std::getenv("VOS_OVERLAY_WINDOW");
  if (!on || std::string(on) != "1") return nullptr;
  return std::make_unique<SdlOverlay>();
}

}  // namespace vos

#endif
