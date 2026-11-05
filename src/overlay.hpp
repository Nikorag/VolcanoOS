// A transparent, click-through, always-on-top window that shows the Volcano OS
// overlay (toasts + the in-game menu) above a running game. The launcher draws
// into a software surface with the same widgets as everything else and hands
// the pixels here.
#pragma once

#include <cstdint>
#include <memory>

namespace vos {

class OverlayWindow {
 public:
  virtual ~OverlayWindow() = default;
  // Pixels are ARGB8888 (SDL_PIXELFORMAT_ARGB8888), non-premultiplied.
  virtual void present(const void* pixels, int width, int height, int pitch) = 0;
  virtual void show(bool visible) = 0;
  virtual bool visible() const = 0;
  // True for an overlay that is a real window which takes keyboard focus when shown (Linux/cage):
  // it must only appear for the menu, never for a passing toast, or it would steal the game's input.
  virtual bool takesFocus() const { return false; }
  // SDL window id of that window (0 if none), so focus events for it can be told apart from the launcher's.
  virtual uint32_t windowId() const { return 0; }
};

// nullptr where there is no native implementation: the launcher then draws the
// overlay inside its own window instead.
std::unique_ptr<OverlayWindow> createOverlayWindow();

}  // namespace vos
