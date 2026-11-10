// Jellyfin server client: discovery, sign-in (Quick Connect or password), library browsing and
// playback reporting. Every call blocks and is thread-safe: run them on the Pool.
//   https://api.jellyfin.org
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "http.hpp"
#include "util.hpp"

namespace vos {

struct JfItem {
  std::string id, name, type;  // type: Movie, Series, Season, Episode, BoxSet, CollectionFolder, ...
  std::string collection, seriesId, seriesName, seasonName, overview, officialRating, mediaSourceId;
  std::string primaryTag, backdropTag, thumbTag, seriesPrimaryTag, parentBackdropId, parentBackdropTag;
  std::vector<std::string> genres;
  int year = 0, seasonNo = 0, episodeNo = 0, runtimeMin = 0, childCount = 0;
  double communityRating = 0;
  int64_t positionTicks = 0, runtimeTicks = 0;
  bool played = false, favorite = false;
  int unplayedCount = 0;
  bool folder = false;  // browsing into it lists more items (Series, Season, BoxSet, library, folder)
  bool playable() const { return type == "Movie" || type == "Episode" || type == "Video" || type == "MusicVideo"; }
  double progress() const { return runtimeTicks > 0 ? double(positionTicks) / double(runtimeTicks) : 0; }
};

struct JfServer {
  std::string name, address, id;  // address: base URL without a trailing slash
};

struct JfSession {
  std::string server, serverName, token, userId, userName;
  bool valid() const { return !server.empty() && !token.empty() && !userId.empty(); }
};

struct JfQuickConnect {
  std::string secret, code;
};

struct JfPage {
  std::vector<JfItem> items;
  int total = 0;
};

template <class T>
struct JfResult {
  bool ok = false;
  int status = 0;  // HTTP status, 0 = could not reach the server
  std::string error;
  T value{};
};

class Jellyfin {
 public:
  explicit Jellyfin(std::string deviceId, std::string deviceName, std::string version);

  void setSession(JfSession s);
  JfSession session() const;
  bool signedIn() const { return session().valid(); }

  // "192.168.1.5:8096", "jellyfin.local" and "https://media.example.com/jf" all become a base URL.
  static std::string normalizeAddress(std::string in);

  // ---- before sign-in (take the server explicitly) -------------------------------------------------
  // LAN discovery (UDP broadcast, ~1.5 s). Blocking; empty if none answered.
  static std::vector<JfServer> discover(int waitMs = 1500);
  JfResult<JfServer> probe(const std::string& address);  // GET /System/Info/Public
  JfResult<bool> quickConnectEnabled(const std::string& server);
  JfResult<JfQuickConnect> quickConnectStart(const std::string& server);
  // ok + value=true once the code has been approved on another device.
  JfResult<bool> quickConnectPoll(const std::string& server, const std::string& secret);
  JfResult<JfSession> signInQuickConnect(const std::string& server, const std::string& secret);
  JfResult<JfSession> signInPassword(const std::string& server, const std::string& user, const std::string& pass);

  // ---- signed in ---------------------------------------------------------------------------------
  JfResult<std::vector<JfItem>> views();                  // libraries
  JfResult<std::vector<JfItem>> resume(int limit = 16);   // Continue Watching
  JfResult<std::vector<JfItem>> nextUp(int limit = 16);
  JfResult<std::vector<JfItem>> latest(const std::string& parentId, int limit = 16);
  JfResult<JfPage> children(const std::string& parentId, int start, int limit, const std::string& search = "",
                            bool recursive = false);
  JfResult<JfPage> search(const std::string& term, int limit = 60);
  JfResult<JfItem> item(const std::string& id);
  JfResult<std::vector<JfItem>> seasons(const std::string& seriesId);
  JfResult<std::vector<JfItem>> episodes(const std::string& seriesId, const std::string& seasonId);
  JfResult<bool> setPlayed(const std::string& id, bool played);

  // Image URL (public endpoint: no token needed). `kind`: Primary, Backdrop, Thumb.
  static std::string avatarUrl(const JfSession& s, int size);
  std::string imageUrl(const std::string& id, const std::string& kind, const std::string& tag, int maxHeight) const;
  // Direct-play URL for mpv. The token travels in a header (see playHeaders), never in the URL.
  std::string streamUrl(const JfItem& it) const;
  std::string authToken() const;

  // ---- playback reporting (ticks are 100 ns) ---------------------------------------------------------
  void reportStart(const JfItem& it, int64_t ticks, const std::string& playSession);
  void reportProgress(const JfItem& it, int64_t ticks, bool paused, const std::string& playSession);
  void reportStopped(const JfItem& it, int64_t ticks, const std::string& playSession);

  static JfItem parseItem(const void* jsonObject);  // nlohmann::json const&; exposed for tests

 private:
  std::string authHeader(bool withToken, const std::string& token) const;
  HttpResponse call(const std::string& method, const std::string& server, const std::string& token,
                    const std::string& path, const std::string& body = "");
  template <class T>
  JfResult<T> fail(const HttpResponse& r) const;

  std::string deviceId_, deviceName_, version_;
  mutable std::mutex m_;
  JfSession s_;
};

}  // namespace vos
