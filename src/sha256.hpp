// Dependency-free SHA-256 — used to verify downloads and derive the PKCE challenge.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vos {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  std::array<uint8_t, 32> finish();
  static std::array<uint8_t, 32> of(const std::string& s);
  static std::string hex(const std::array<uint8_t, 32>& d);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};

}  // namespace vos
