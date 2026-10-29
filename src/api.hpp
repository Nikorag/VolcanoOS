// Typed client for the Zonto platform API (https://www.nikorag.co.uk).
// Every call is blocking and thread-safe: run them on the Pool.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "http.hpp"

namespace vos {

template <class T>
struct Result {
  bool ok = false;
  int status = 0;  // HTTP status, 0 = could not reach the server
  std::string error;
  T value{};
  bool unauthorized() const { return status == 401; }
  bool offline() const { return status == 0; }
};

struct Player {
  int64_t id = 0;
  std::string gamertag;
  int level = 0;
};

struct BuildInfo {
  std::string version, archiveFormat, sha256, exePath, launchArgs;
  uint64_t sizeBytes = 0;
};

struct GameEntry {
  std::string slug, name, blurb, tags, playUrl, logoUrl, heroUrl, lastPlayedAt;
  int minutesPlayed = 0, progressPct = 0, achUnlocked = 0, achTotal = 0;
  bool selfReportsPlaytime = false;
  bool hasBuild = false;
  BuildInfo build;
  bool isWeb() const { return !playUrl.empty(); }
};

struct BuildDownload {
  BuildInfo info;
  std::string url;
};

struct LaunchInfo {
  std::string launchUrl;  // ticketed play URL for web titles, empty for native
};

// The build slot this machine asks for. ZONTO_PLATFORM / ZONTO_ARCH override it
// so the install loop can be exercised against another target in development.
std::pair<std::string, std::string> buildTarget();

class Api {
 public:
  Api();
  const std::string& base() const { return base_; }
  void setToken(std::string t);
  std::string token() const;
  bool hasToken() const { return !token().empty(); }

  Result<Player> me() const;
  // `raw` receives the response body so the caller can cache it for offline use.
  Result<std::vector<GameEntry>> library(std::string* raw = nullptr) const;
  static std::vector<GameEntry> parseLibrary(const std::string& json);

  Result<BuildDownload> buildDownload(const std::string& slug) const;
  Result<LaunchInfo> recordLaunch(const std::string& slug) const;
  Result<bool> presence(const std::optional<std::string>& slug) const;
  Result<bool> reportMinutes(const std::string& slug, int minutes) const;
  Result<bool> syncDevice(const std::string& jsonBody) const;
  // Generic JSON call for the native pages. `body` is the response text.
  Result<std::string> request(const char* method, const std::string& path, const std::string& jsonBody = {}) const;

 private:
  HttpResponse call(const char* method, const std::string& path, const std::string& body = {}) const;
  template <class T>
  static Result<T> failure(const HttpResponse& r, const std::string& what);

  std::string base_;
  mutable std::mutex m_;
  std::string token_;
};

}  // namespace vos
