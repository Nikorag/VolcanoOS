#include "tokenstore.hpp"

#include <sys/stat.h>

#include "util.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace vos {

#ifdef __APPLE__
namespace tokenstore {
// Implemented in tokenstore_mac.cpp.
std::optional<std::string> keychainLoad();
bool keychainSave(const std::string& token);
void keychainClear();
}  // namespace tokenstore
#endif

namespace tokenstore {

static fs::path tokenFile() { return dataDir() / "token"; }

static std::optional<std::string> fileLoad() {
  std::string s;
  if (!readFile(tokenFile(), s)) return std::nullopt;
  s = trim(s);
  if (s.empty()) return std::nullopt;
  return s;
}

static bool fileSave(const std::string& token) {
  if (!writeFileAtomic(tokenFile(), token)) return false;
#ifndef _WIN32
  chmod(tokenFile().string().c_str(), 0600);
#endif
  return true;
}

std::optional<std::string> load() {
#ifdef __APPLE__
  if (auto t = keychainLoad()) return t;
#endif
  return fileLoad();
}

bool save(const std::string& token) {
#ifdef __APPLE__
  if (keychainSave(token)) {
    std::error_code ec;
    fs::remove(tokenFile(), ec);
    return true;
  }
#endif
  return fileSave(token);
}

void clear() {
#ifdef __APPLE__
  keychainClear();
#endif
  std::error_code ec;
  fs::remove(tokenFile(), ec);
}

}  // namespace tokenstore
}  // namespace vos
