// A transparent, click-through, always-on-top window that shows the Volcano OS
// overlay (toasts + the in-game menu) above a running game. The launcher draws
// into a software surface with the same widgets as everything else and hands
// the pixels here.
#pragma once

#include <memory>

namespace vos {

class OverlayWindow {
 public:
  virtual ~OverlayWindow() = default;
  // Pixels are ARGB8888 (SDL_PIXELFORMAT_ARGB8888), non-premultiplied.
  virtual void present(const void* pixels, int width, int height, int pitch) = 0;
  virtual void show(bool visible) = 0;
  virtual bool visible() const = 0;
};

// nullptr where there is no native implementation: the launcher then draws the
// overlay inside its own window instead.
std::unique_ptr<OverlayWindow> createOverlayWindow();

}  // namespace vos
