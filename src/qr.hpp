// Minimal QR Code encoder (byte mode, versions 1-10, error correction L or M):
// enough for a sign-in URL. Follows ISO/IEC 18004; no dependencies.
#pragma once

#include <string>
#include <vector>

namespace vos {

struct QrCode {
  int size = 0;                  // modules per side; 0 = failed to encode
  std::vector<bool> modules;     // row-major, true = dark
  bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<size_t>(y) * size + x]; }
};

// Smallest version that fits. Returns size 0 if `text` is too long (> ~213 bytes at level L).
QrCode encodeQr(const std::string& text, bool highEcc = true);

}  // namespace vos
