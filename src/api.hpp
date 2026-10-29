// Typed client for the Zonto platform API (https://www.nikorag.co.uk).
// Every call is blocking and thread-safe: run them on the Pool.
#pragma once

#include <cstdint>
#include <functional>
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

// What a sign-in yields. Access tokens last about an hour; the refresh token
// (single use, rotated on every refresh) keeps the console signed in for as long
// as it is used at least once every ~60 days. `refresh` is empty for legacy
// tokens, and `expiresAt` (unix seconds) is 0 when unknown.
struct Credentials {
  std::string access, refresh;
  int64_t expiresAt = 0;
  bool empty() const { return access.empty(); }
  std::string serialize() const;                        // what goes in the keychain/token file
  static Credentials parse(const std::string& stored);  // also accepts a bare legacy access token
};

class Api {
 public:
  Api();
  const std::string& base() const { return base_; }
  void setToken(std::string t) { setCredentials(Credentials{std::move(t), {}, 0}); }  // "" signs out
  void setCredentials(Credentials c);
  std::string token() const;
  bool hasToken() const { return !token().empty(); }

  // Persist rotated credentials. Called, before the new access token is used,
  // from whichever worker thread triggered the refresh.
  std::function<void(const Credentials&)> onCredentials;
  // The refresh token was rejected (expired, revoked, replayed): the player must sign in again.
  // Called from a worker thread.
  std::function<void()> onSessionLost;

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
  void storeCredentials(Credentials c) const;
  bool refreshTokens(const std::string& staleAccess) const;
  void ensureFresh() const;
  HttpResponse call(const char* method, const std::string& path, const std::string& body = {}) const;
  template <class T>
  static Result<T> failure(const HttpResponse& r, const std::string& what);

  std::string base_;
  mutable std::mutex m_;
  mutable Credentials creds_;
  mutable std::mutex refreshM_;       // one refresh at a time: tokens are single-use
  mutable double nextRefreshAt_ = 0;  // back-off after a refresh whose outcome is unknown
};

}  // namespace vos
