#include "api.hpp"

#include <cstdlib>
#include <ctime>

#include "json.hpp"

namespace vos {

using nlohmann::json;

std::pair<std::string, std::string> buildTarget() {
  std::string platform, arch;
  if (const char* p = std::getenv("ZONTO_PLATFORM"); p && *p) platform = p;
  else {
#if defined(__APPLE__)
    platform = "darwin";
#elif defined(_WIN32)
    platform = "windows";
#else
    platform = "linux";
#endif
  }
  if (const char* a = std::getenv("ZONTO_ARCH"); a && *a) arch = a;
  else {
#if defined(__aarch64__) || defined(_M_ARM64)
    arch = "aarch64";
#else
    arch = "x86_64";
#endif
  }
  return {platform, arch};
}

Api::Api() {
  const char* o = std::getenv("ZONTO_API");
  base_ = (o && *o) ? o : "https://www.nikorag.co.uk";
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

std::string Credentials::serialize() const {
  return json{{"v", 1}, {"access", access}, {"refresh", refresh}, {"exp", expiresAt}}.dump();
}

Credentials Credentials::parse(const std::string& stored) {
  Credentials c;
  if (!stored.empty() && stored[0] == '{') {
    json j = json::parse(stored, nullptr, false);
    if (j.is_object()) {
      if (j.contains("access") && j["access"].is_string()) c.access = j["access"].get<std::string>();
      if (j.contains("refresh") && j["refresh"].is_string()) c.refresh = j["refresh"].get<std::string>();
      if (j.contains("exp") && j["exp"].is_number_integer()) c.expiresAt = j["exp"].get<int64_t>();
    }
    return c;
  }
  c.access = stored;  // legacy: a bare 30-day bearer
  return c;
}

void Api::setCredentials(Credentials c) { storeCredentials(std::move(c)); }

void Api::storeCredentials(Credentials c) const {
  std::lock_guard<std::mutex> l(m_);
  creds_ = std::move(c);
}

std::string Api::token() const {
  std::lock_guard<std::mutex> l(m_);
  return creds_.access;
}

// Refresh a minute early so a request never leaves with a token that dies in flight.
void Api::ensureFresh() const {
  std::string stale;
  {
    std::lock_guard<std::mutex> l(m_);
    if (creds_.refresh.empty() || creds_.expiresAt == 0) return;
    if (static_cast<int64_t>(std::time(nullptr)) + 60 < creds_.expiresAt) return;
    stale = creds_.access;
  }
  refreshTokens(stale);
}

// Returns true when `staleAccess` is no longer the current token because it was
// refreshed, by us or by another thread. The server's rules: each refresh token
// works once, and presenting a used one revokes the whole chain.
bool Api::refreshTokens(const std::string& staleAccess) const {
  std::lock_guard<std::mutex> rl(refreshM_);
  Credentials cur;
  {
    std::lock_guard<std::mutex> l(m_);
    cur = creds_;
  }
  if (cur.access != staleAccess) return true;  // another thread already rotated it
  if (cur.refresh.empty()) return false;
  // After a refresh with an unknown outcome, don't hammer. Never resend immediately either:
  // if the server did rotate, the old token is now a replay.
  if (nowSeconds() < nextRefreshAt_) return false;

  HttpRequest r;
  r.method = "POST";
  r.url = base_ + "/api/oauth/token";
  r.contentType = "application/json";
  r.timeoutSec = 15;
  r.body = json{{"grant_type", "refresh_token"}, {"refresh_token", cur.refresh}, {"client_id", "zonto-launcher"}}.dump();
  HttpResponse res = httpRequest(r);

  if (res.status == 0 || res.status >= 500) {  // outcome unknown: keep what we have, try again later
    nextRefreshAt_ = nowSeconds() + 30;
    logf("token refresh didn't complete (%s); will retry later", res.status == 0 ? res.error.c_str() : "server error");
    return false;
  }
  json j = json::parse(res.body, nullptr, false);
  if (res.ok() && j.is_object() && j.contains("access_token") && j["access_token"].is_string()) {
    Credentials next;
    next.access = j["access_token"].get<std::string>();
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) next.refresh = j["refresh_token"].get<std::string>();
    double ttl = (j.contains("expires_in") && j["expires_in"].is_number()) ? j["expires_in"].get<double>() : 3600;
    next.expiresAt = static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(ttl);
    // Persist first: the old refresh token is spent, so losing the new one would sign the player out.
    if (onCredentials) onCredentials(next);
    storeCredentials(next);
    nextRefreshAt_ = 0;
    return true;
  }
  std::string err = j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "";
  if (err == "invalid_grant") {
    logf("refresh token rejected; signing out");
    storeCredentials({});
    if (onSessionLost) onSessionLost();
  } else {
    logf("token refresh failed (HTTP %ld %s)", res.status, err.c_str());
    nextRefreshAt_ = nowSeconds() + 30;
  }
  return false;
}

HttpResponse Api::call(const char* method, const std::string& path, const std::string& body) const {
  ensureFresh();
  auto send = [&] {
    HttpRequest r;
    r.method = method;
    r.url = base_ + path;
    r.bearer = token();
    r.body = body;
    if (!body.empty()) r.contentType = "application/json";
    return httpRequest(r);
  };
  const std::string used = token();
  HttpResponse res = send();
  // Expired earlier than we thought (clock skew, e.g. a Pi booting before NTP): refresh and retry once.
  if (res.status == 401 && !used.empty() && refreshTokens(used)) res = send();
  return res;
}

template <class T>
Result<T> Api::failure(const HttpResponse& r, const std::string& what) {
  Result<T> out;
  out.status = static_cast<int>(r.status);
  if (r.status == 0) out.error = "Can't reach Zonto (" + r.error + ")";
  else if (r.status == 401) out.error = "Your sign-in expired. Sign in again.";
  else {
    out.error = what + " failed (HTTP " + std::to_string(r.status) + ")";
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_string()) out.error = j["error"].get<std::string>();
  }
  return out;
}

static std::string str(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
static int64_t num(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number()) ? it->get<int64_t>() : 0;
}

Result<Player> Api::me() const {
  HttpResponse r = call("GET", "/api/auth/me");
  if (!r.ok()) return failure<Player>(r, "Account lookup");
  json j = json::parse(r.body, nullptr, false);
  Result<Player> out;
  out.ok = j.is_object();
  out.status = static_cast<int>(r.status);
  if (!out.ok) {
    out.error = "Unreadable account response";
    return out;
  }
  out.value.id = num(j, "id");
  out.value.gamertag = str(j, "gamertag");
  out.value.level = static_cast<int>(num(j, "level"));
  return out;
}

static BuildInfo parseBuild(const json& b) {
  BuildInfo i;
  i.version = str(b, "version");
  i.archiveFormat = str(b, "archiveFormat");
  i.sha256 = str(b, "sha256");
  i.exePath = str(b, "exePath");
  i.launchArgs = str(b, "launchArgs");
  i.sizeBytes = static_cast<uint64_t>(num(b, "sizeBytes"));
  return i;
}

std::vector<GameEntry> Api::parseLibrary(const std::string& body) {
  std::vector<GameEntry> out;
  json j = json::parse(body, nullptr, false);
  if (!j.is_array()) return out;
  for (const auto& e : j) {
    if (!e.is_object()) continue;
    GameEntry g;
    g.slug = str(e, "slug");
    if (g.slug.empty()) continue;
    g.name = str(e, "name");
    g.blurb = str(e, "blurb");
    g.tags = str(e, "tags");
    g.playUrl = str(e, "playUrl");
    g.logoUrl = str(e, "logoUrl");
    g.heroUrl = str(e, "heroUrl");
    g.lastPlayedAt = str(e, "lastPlayedAt");
    g.minutesPlayed = static_cast<int>(num(e, "minutesPlayed"));
    g.progressPct = static_cast<int>(num(e, "progressPct"));
    if (auto a = e.find("achievements"); a != e.end() && a->is_object()) {
      g.achUnlocked = static_cast<int>(num(*a, "unlocked"));
      g.achTotal = static_cast<int>(num(*a, "total"));
    }
    if (auto s = e.find("selfReportsPlaytime"); s != e.end() && s->is_boolean()) g.selfReportsPlaytime = s->get<bool>();
    if (auto b = e.find("build"); b != e.end() && b->is_object()) {
      g.build = parseBuild(*b);
      g.hasBuild = !g.build.version.empty() && !g.build.exePath.empty();
    }
    out.push_back(std::move(g));
  }
  return out;
}

Result<std::vector<GameEntry>> Api::library(std::string* raw) const {
  auto [platform, arch] = buildTarget();
  HttpResponse r = call("GET", "/api/library?platform=" + urlEncode(platform) + "&arch=" + urlEncode(arch));
  if (!r.ok()) return failure<std::vector<GameEntry>>(r, "Library request");
  Result<std::vector<GameEntry>> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  out.value = parseLibrary(r.body);
  if (raw) *raw = r.body;
  return out;
}

Result<BuildDownload> Api::buildDownload(const std::string& slug) const {
  auto [platform, arch] = buildTarget();
  HttpResponse r =
      call("GET", "/api/library/" + urlEncode(slug) + "/build?platform=" + urlEncode(platform) + "&arch=" + urlEncode(arch));
  if (r.status == 403) {
    Result<BuildDownload> o;
    o.status = 403;
    o.error = "That title isn't in your library.";
    return o;
  }
  if (r.status == 404) {
    Result<BuildDownload> o;
    o.status = 404;
    o.error = "No " + platform + "/" + arch + " build is published for this title.";
    return o;
  }
  if (!r.ok()) return failure<BuildDownload>(r, "Download request");
  json j = json::parse(r.body, nullptr, false);
  Result<BuildDownload> out;
  out.status = static_cast<int>(r.status);
  if (!j.is_object() || str(j, "url").empty()) {
    out.error = "Unreadable download response";
    return out;
  }
  out.ok = true;
  out.value.info = parseBuild(j);
  out.value.url = str(j, "url");
  return out;
}

Result<LaunchInfo> Api::recordLaunch(const std::string& slug) const {
  HttpResponse r = call("POST", "/api/library/" + urlEncode(slug) + "/launch", "{}");
  if (!r.ok()) return failure<LaunchInfo>(r, "Launch");
  json j = json::parse(r.body, nullptr, false);
  Result<LaunchInfo> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  if (j.is_object()) out.value.launchUrl = str(j, "launchUrl");  // null -> ""
  return out;
}

Result<bool> Api::presence(const std::optional<std::string>& slug) const {
  // Three cases the server distinguishes: slug = now playing, explicit null =
  // stopped, omitted = just a heartbeat. Stopping must send a literal null.
  json body = {{"gameSlug", slug ? json(*slug) : json(nullptr)}};
  HttpResponse r = call("POST", "/api/presence", body.dump());
  if (!r.ok()) return failure<bool>(r, "Presence");
  Result<bool> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  return out;
}

Result<bool> Api::reportMinutes(const std::string& slug, int minutes) const {
  json body = {{"addMinutes", minutes}};
  HttpResponse r = call("POST", "/api/library/" + urlEncode(slug) + "/progress", body.dump());
  if (!r.ok()) return failure<bool>(r, "Playtime report");
  Result<bool> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  return out;
}

Result<bool> Api::syncDevice(const std::string& jsonBody) const {
  HttpResponse r = call("PUT", "/api/devices", jsonBody);
  if (!r.ok()) return failure<bool>(r, "Device sync");
  Result<bool> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  return out;
}

Result<std::string> Api::request(const char* method, const std::string& path, const std::string& jsonBody) const {
  HttpResponse r = call(method, path, jsonBody);
  if (!r.ok()) return failure<std::string>(r, "Request");
  Result<std::string> out;
  out.ok = true;
  out.status = static_cast<int>(r.status);
  out.value = r.body;
  return out;
}

}  // namespace vos
