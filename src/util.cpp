#include "util.hpp"

#include <SDL.h>

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace vos {

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[volcano] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}

static fs::path computeDataDir() {
  if (const char* o = std::getenv("VOS_DATA_DIR"); o && *o) return fs::path(o);
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  fs::path p = fs::path(home ? home : ".") / "Library" / "Application Support" / "Zonto" / "VolcanoOS";
#elif defined(_WIN32)
  const char* la = std::getenv("LOCALAPPDATA");
  fs::path p = fs::path(la ? la : ".") / "Zonto" / "VolcanoOS";
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "share";
  fs::path p = base / "zonto" / "volcanoos";
#endif
  return p;
}

const fs::path& dataDir() {
  static fs::path p = [] {
    fs::path d = computeDataDir();
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
  }();
  return p;
}

const fs::path& assetDir() {
  static fs::path p = [] {
    if (char* base = SDL_GetBasePath()) {
      fs::path a = fs::path(base) / "assets";
      SDL_free(base);
      std::error_code ec;
      if (fs::exists(a / "fonts", ec)) return a;
    }
    return fs::path(VOS_ASSET_DIR);
  }();
  return p;
}

fs::path gamesDir() {
  fs::path d = dataDir() / "games";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

std::string upper(std::string s) {
  for (auto& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string humanBytes(uint64_t b) {
  char buf[32];
  const double kb = 1024.0, mb = kb * 1024, gb = mb * 1024;
  if (b >= gb) std::snprintf(buf, sizeof buf, "%.1f GB", b / gb);
  else if (b >= mb) std::snprintf(buf, sizeof buf, "%.0f MB", b / mb);
  else if (b >= kb) std::snprintf(buf, sizeof buf, "%.0f KB", b / kb);
  else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

std::string humanPlaytime(int minutes) {
  if (minutes <= 0) return "NOT PLAYED";
  char buf[32];
  if (minutes < 60) std::snprintf(buf, sizeof buf, "%dM", minutes);
  else std::snprintf(buf, sizeof buf, "%dH %02dM", minutes / 60, minutes % 60);
  return buf;
}

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string base64Url(const uint8_t* d, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += t[v & 63];
  }
  if (i + 1 == n) {
    uint32_t v = d[i] << 16;
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
  } else if (i + 2 == n) {
    uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
  }
  return out;
}

std::string slugify(const std::string& s) {
  std::string out;
  for (unsigned char c : s) out += (std::isalnum(c) ? static_cast<char>(c) : '-');
  return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool readFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool writeFileAtomic(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

bool randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) return false;
  size_t got = std::fread(out, 1, n, f);
  std::fclose(f);
  return got == n;
#endif
}

std::string randomToken(size_t bytes) {
  std::vector<uint8_t> b(bytes);
  if (!randomBytes(b.data(), bytes)) {
    // Never fall back to something guessable for a security token.
    logf("no secure randomness available");
    return {};
  }
  return base64Url(b.data(), b.size());
}

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- threading -------------------------------------------------------------

void MainQueue::post(std::function<void()> fn) {
  std::lock_guard<std::mutex> l(m_);
  q_.push(std::move(fn));
}

void MainQueue::drain() {
  std::queue<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> l(m_);
    std::swap(local, q_);
  }
  while (!local.empty()) {
    local.front()();
    local.pop();
  }
}

Pool::Pool(unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    threads_.emplace_back([this] {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> l(m_);
          cv_.wait(l, [this] { return stop_ || !q_.empty(); });
          if (stop_ && q_.empty()) return;
          job = std::move(q_.front());
          q_.pop();
        }
        job();
      }
    });
  }
}

Pool::~Pool() {
  {
    std::lock_guard<std::mutex> l(m_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void Pool::submit(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> l(m_);
    q_.push(std::move(job));
  }
  cv_.notify_one();
}

}  // namespace vos
