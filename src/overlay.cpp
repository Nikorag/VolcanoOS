#include "overlay.hpp"

namespace vos {

#ifndef __APPLE__
std::unique_ptr<OverlayWindow> createOverlayWindow() { return nullptr; }
#endif

}  // namespace vos
