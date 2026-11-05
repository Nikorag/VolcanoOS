#include "overlay.hpp"

namespace vos {

#if !defined(__APPLE__) && !defined(__linux__)
std::unique_ptr<OverlayWindow> createOverlayWindow() { return nullptr; }
#endif

}  // namespace vos
