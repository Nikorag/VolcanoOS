#include "qr.hpp"

#include <algorithm>
#include <cstdint>

namespace vos {

namespace {

constexpr int kMaxVersion = 10;
// Index = version (0 unused). Level L then M.
constexpr int kEccPerBlock[2][kMaxVersion + 1] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26}};
constexpr int kNumBlocks[2][kMaxVersion + 1] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5}};

int rawDataModules(int ver) {
  int r = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    int na = ver / 7 + 2;
    r -= (25 * na - 10) * na - 55;
    if (ver >= 7) r -= 36;
  }
  return r;
}

int dataCodewords(int ver, int e) { return rawDataModules(ver) / 8 - kEccPerBlock[e][ver] * kNumBlocks[e][ver]; }

bool bitAt(unsigned v, int i) { return ((v >> i) & 1) != 0; }

// ---- Reed-Solomon over GF(256), polynomial 0x11D ----
uint8_t gfMul(uint8_t x, uint8_t y) {
  int z = 0;
  for (int i = 7; i >= 0; --i) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return static_cast<uint8_t>(z);
}

std::vector<uint8_t> rsDivisor(int degree) {
  std::vector<uint8_t> r(degree, 0);
  r[degree - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; ++i) {
    for (int j = 0; j < degree; ++j) {
      r[j] = gfMul(r[j], root);
      if (j + 1 < degree) r[j] ^= r[j + 1];
    }
    root = gfMul(root, 0x02);
  }
  return r;
}

std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
  std::vector<uint8_t> r(div.size(), 0);
  for (uint8_t b : data) {
    uint8_t factor = b ^ r.front();
    r.erase(r.begin());
    r.push_back(0);
    for (size_t i = 0; i < r.size(); ++i) r[i] ^= gfMul(div[i], factor);
  }
  return r;
}

class Builder {
 public:
  Builder(int ver, int ecc) : ver_(ver), ecc_(ecc), size_(ver * 4 + 17) {
    mod_.assign(static_cast<size_t>(size_) * size_, false);
    fn_.assign(static_cast<size_t>(size_) * size_, false);
  }

  QrCode build(const std::vector<uint8_t>& data) {
    drawFunctionPatterns();
    drawCodewords(addEccAndInterleave(data));
    int best = 0, bestPenalty = 1 << 30;
    for (int m = 0; m < 8; ++m) {
      applyMask(m);
      drawFormatBits(m);
      int p = penalty();
      if (p < bestPenalty) {
        best = m;
        bestPenalty = p;
      }
      applyMask(m);  // undo (XOR)
    }
    applyMask(best);
    drawFormatBits(best);
    QrCode q;
    q.size = size_;
    q.modules = mod_;
    return q;
  }

 private:
  bool get(int x, int y) const { return mod_[static_cast<size_t>(y) * size_ + x]; }
  void setFn(int x, int y, bool dark) {
    mod_[static_cast<size_t>(y) * size_ + x] = dark;
    fn_[static_cast<size_t>(y) * size_ + x] = true;
  }
  bool isFn(int x, int y) const { return fn_[static_cast<size_t>(y) * size_ + x]; }

  void drawFunctionPatterns() {
    for (int i = 0; i < size_; ++i) {  // timing
      setFn(6, i, i % 2 == 0);
      setFn(i, 6, i % 2 == 0);
    }
    drawFinder(3, 3);
    drawFinder(size_ - 4, 3);
    drawFinder(3, size_ - 4);
    auto pos = alignmentPositions();
    int n = static_cast<int>(pos.size());
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
        drawAlignment(pos[i], pos[j]);
      }
    drawFormatBits(0);  // reserve; rewritten once the mask is known
    drawVersion();
  }

  std::vector<int> alignmentPositions() const {
    if (ver_ == 1) return {};
    int na = ver_ / 7 + 2;
    int step = (ver_ * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
    std::vector<int> r(na);
    r[0] = 6;
    for (int i = na - 1, p = size_ - 7; i >= 1; --i, p -= step) r[i] = p;
    return r;
  }

  void drawFinder(int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx) {
        int d = std::max(std::abs(dx), std::abs(dy));
        int x = cx + dx, y = cy + dy;
        if (x >= 0 && x < size_ && y >= 0 && y < size_) setFn(x, y, d != 2 && d != 4);
      }
  }

  void drawAlignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) setFn(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
  }

  void drawFormatBits(int mask) {
    // Level L = 1, M = 0 in the format field.
    unsigned data = static_cast<unsigned>((ecc_ == 0 ? 1 : 0) << 3 | mask);
    unsigned rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    unsigned bits = (data << 10 | rem) ^ 0x5412;
    for (int i = 0; i <= 5; ++i) setFn(8, i, bitAt(bits, i));
    setFn(8, 7, bitAt(bits, 6));
    setFn(8, 8, bitAt(bits, 7));
    setFn(7, 8, bitAt(bits, 8));
    for (int i = 9; i < 15; ++i) setFn(14 - i, 8, bitAt(bits, i));
    for (int i = 0; i < 8; ++i) setFn(size_ - 1 - i, 8, bitAt(bits, i));
    for (int i = 8; i < 15; ++i) setFn(8, size_ - 15 + i, bitAt(bits, i));
    setFn(8, size_ - 8, true);  // always-dark module
  }

  void drawVersion() {
    if (ver_ < 7) return;
    unsigned rem = static_cast<unsigned>(ver_);
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    unsigned bits = static_cast<unsigned>(ver_) << 12 | rem;
    for (int i = 0; i < 18; ++i) {
      bool b = bitAt(bits, i);
      int a = size_ - 11 + i % 3, c = i / 3;
      setFn(a, c, b);
      setFn(c, a, b);
    }
  }

  std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data) const {
    int numBlocks = kNumBlocks[ecc_][ver_], eccLen = kEccPerBlock[ecc_][ver_];
    int raw = rawDataModules(ver_) / 8;
    int numShort = numBlocks - raw % numBlocks, shortLen = raw / numBlocks;
    std::vector<std::vector<uint8_t>> blocks;
    auto div = rsDivisor(eccLen);
    for (int i = 0, k = 0; i < numBlocks; ++i) {
      int datLen = shortLen - eccLen + (i < numShort ? 0 : 1);
      std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + datLen);
      k += datLen;
      auto ec = rsRemainder(dat, div);
      if (i < numShort) dat.push_back(0);
      dat.insert(dat.end(), ec.begin(), ec.end());
      blocks.push_back(std::move(dat));
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i < blocks[0].size(); ++i)
      for (int j = 0; j < numBlocks; ++j)
        if (static_cast<int>(i) != shortLen - eccLen || j >= numShort) out.push_back(blocks[j][i]);
    return out;
  }

  void drawCodewords(const std::vector<uint8_t>& data) {
    size_t i = 0;
    for (int right = size_ - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;
      for (int vert = 0; vert < size_; ++vert)
        for (int j = 0; j < 2; ++j) {
          int x = right - j;
          bool upward = ((right + 1) & 2) == 0;
          int y = upward ? size_ - 1 - vert : vert;
          if (!isFn(x, y) && i < data.size() * 8) {
            mod_[static_cast<size_t>(y) * size_ + x] = bitAt(data[i >> 3], 7 - static_cast<int>(i & 7));
            ++i;
          }
        }
    }
  }

  void applyMask(int mask) {
    for (int y = 0; y < size_; ++y)
      for (int x = 0; x < size_; ++x) {
        bool inv = false;
        switch (mask) {
          case 0: inv = (x + y) % 2 == 0; break;
          case 1: inv = y % 2 == 0; break;
          case 2: inv = x % 3 == 0; break;
          case 3: inv = (x + y) % 3 == 0; break;
          case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
          case 5: inv = x * y % 2 + x * y % 3 == 0; break;
          case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
          case 7: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (inv && !isFn(x, y)) mod_[static_cast<size_t>(y) * size_ + x] = !get(x, y);
      }
  }

  // Simplified ISO penalty: runs, 2x2 blocks, finder-like patterns, dark balance.
  int penalty() const {
    int p = 0;
    for (int pass = 0; pass < 2; ++pass)
      for (int a = 0; a < size_; ++a) {
        int run = 1;
        for (int b = 1; b < size_; ++b) {
          bool cur = pass ? get(a, b) : get(b, a), prev = pass ? get(a, b - 1) : get(b - 1, a);
          if (cur == prev) {
            ++run;
            if (run == 5) p += 3;
            else if (run > 5) ++p;
          } else {
            run = 1;
          }
        }
        // 1:1:3:1:1 finder-like pattern with 4 light modules on a side.
        for (int b = 0; b + 10 < size_; ++b) {
          auto at = [&](int k) { return pass ? get(a, b + k) : get(b + k, a); };
          bool core = at(0) && !at(1) && at(2) && at(3) && at(4) && !at(5) && at(6);
          if (core && ((!at(7) && !at(8) && !at(9) && !at(10)))) p += 40;
          bool rev = at(4) && !at(5) && at(6) && at(7) && at(8) && !at(9) && at(10);
          if (rev && !at(0) && !at(1) && !at(2) && !at(3)) p += 40;
        }
      }
    for (int y = 0; y + 1 < size_; ++y)
      for (int x = 0; x + 1 < size_; ++x) {
        bool c = get(x, y);
        if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) p += 3;
      }
    int dark = 0;
    for (int y = 0; y < size_; ++y)
      for (int x = 0; x < size_; ++x) dark += get(x, y);
    int total = size_ * size_;
    int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    return p + std::max(0, k) * 10;
  }

  int ver_, ecc_, size_;
  std::vector<bool> mod_, fn_;
};

}  // namespace

QrCode encodeQr(const std::string& text, bool highEcc) {
  const int e = highEcc ? 1 : 0;
  const size_t n = text.size();
  for (int ver = 1; ver <= kMaxVersion; ++ver) {
    const int countBits = ver <= 9 ? 8 : 16;
    const size_t capBits = static_cast<size_t>(dataCodewords(ver, e)) * 8;
    const size_t needBits = 4 + countBits + n * 8;
    if (needBits > capBits || (countBits == 8 && n > 255)) continue;

    std::vector<bool> bits;
    auto put = [&](unsigned v, int len) {
      for (int i = len - 1; i >= 0; --i) bits.push_back(bitAt(v, i));
    };
    put(0x4, 4);  // byte mode
    put(static_cast<unsigned>(n), countBits);
    for (unsigned char c : text) put(c, 8);
    put(0, static_cast<int>(std::min<size_t>(4, capBits - bits.size())));  // terminator
    while (bits.size() % 8) bits.push_back(false);
    std::vector<uint8_t> data(capBits / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
      if (bits[i]) data[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
    for (size_t i = bits.size() / 8, pad = 0xEC; i < data.size(); ++i, pad ^= 0xEC ^ 0x11) data[i] = static_cast<uint8_t>(pad);
    return Builder(ver, e).build(data);
  }
  return {};
}

}  // namespace vos
