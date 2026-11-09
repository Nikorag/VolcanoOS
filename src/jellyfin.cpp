#include "jellyfin.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "json.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vos {

using nlohmann::json;

namespace {

std::string str(const json& j, const char* k) {
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
int64_t num(const json& j, const char* k) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}

std::string quote(const std::string& v) {  // header parameter value
  std::string o = "\"";
  for (char c : v) o += (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
  return o + "\"";
}

}  // namespace

Jellyfin::Jellyfin(std::string deviceId, std::string deviceName, std::string version)
    : deviceId_(std::move(deviceId)), deviceName_(std::move(deviceName)), version_(std::move(version)) {}

void Jellyfin::setSession(JfSession s) {
  std::lock_guard<std::mutex> l(m_);
  s_ = std::move(s);
}
JfSession Jellyfin::session() const {
  std::lock_guard<std::mutex> l(m_);
  return s_;
}
std::string Jellyfin::authToken() const { return session().token; }

std::string Jellyfin::normalizeAddress(std::string in) {
  auto trim = [](std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s.erase(0, i);
  };
  trim(in);
  if (in.empty()) return {};
  if (in.find("://") == std::string::npos) in = "http://" + in;
  while (!in.empty() && in.back() == '/') in.pop_back();
  // No port given on a plain-http address: Jellyfin's default.
  if (in.rfind("http://", 0) == 0) {
    std::string host = in.substr(7);
    auto slash = host.find('/');
    std::string hostOnly = host.substr(0, slash);
    if (hostOnly.find(':') == std::string::npos) in = "http://" + hostOnly + ":8096" + (slash == std::string::npos ? "" : host.substr(slash));
  }
  return in;
}

std::string Jellyfin::authHeader(bool withToken, const std::string& token) const {
  std::string h = "Authorization: MediaBrowser Client=" + quote("Volcano OS") + ", Device=" + quote(deviceName_) +
                  ", DeviceId=" + quote(deviceId_) + ", Version=" + quote(version_);
  if (withToken && !token.empty()) h += ", Token=" + quote(token);
  return h;
}

HttpResponse Jellyfin::call(const std::string& method, const std::string& server, const std::string& token,
                            const std::string& path, const std::string& body) {
  HttpRequest rq;
  rq.method = method;
  rq.url = server + path;
  rq.timeoutSec = 20;
  rq.headers.push_back(authHeader(true, token));
  if (!body.empty()) {
    rq.body = body;
    rq.contentType = "application/json";
  }
  return httpRequest(rq);
}

template <class T>
JfResult<T> Jellyfin::fail(const HttpResponse& r) const {
  JfResult<T> o;
  o.status = static_cast<int>(r.status);
  if (r.status == 0) o.error = r.error.empty() ? "Could not reach the server" : r.error;
  else if (r.status == 401) o.error = "Not authorised. Sign in again.";
  else if (r.status == 404) o.error = "Not found";
  else {
    json j = json::parse(r.body, nullptr, false);
    o.error = (j.is_object() && !str(j, "Message").empty()) ? str(j, "Message") : "Server error " + std::to_string(r.status);
  }
  return o;
}

// ---- discovery ------------------------------------------------------------------------------------

std::vector<JfServer> Jellyfin::discover(int waitMs) {
  std::vector<JfServer> out;
#ifndef _WIN32
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return out;
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(7359);
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  const char* msg = "who is JellyfinServer?";
  ::sendto(fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
  const double end = nowSeconds() + waitMs / 1000.0;
  for (;;) {
    int left = static_cast<int>((end - nowSeconds()) * 1000);
    if (left <= 0) break;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, left) <= 0) break;
    char buf[2048];
    ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) continue;
    json j = json::parse(std::string(buf, static_cast<size_t>(n)), nullptr, false);
    if (!j.is_object()) continue;
    JfServer s{str(j, "Name"), str(j, "Address"), str(j, "Id")};
    while (!s.address.empty() && s.address.back() == '/') s.address.pop_back();
    if (s.address.empty()) continue;
    bool dup = false;
    for (auto& e : out) dup |= e.id == s.id && e.address == s.address;
    if (!dup) out.push_back(s);
  }
  ::close(fd);
#else
  (void)waitMs;
#endif
  return out;
}

JfResult<JfServer> Jellyfin::probe(const std::string& address) {
  HttpResponse r = call("GET", address, "", "/System/Info/Public");
  if (!r.ok()) return fail<JfServer>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<JfServer> o;
  if (!j.is_object() || str(j, "ProductName").find("Jellyfin") == std::string::npos) {
    o.error = "That doesn't look like a Jellyfin server";
    return o;
  }
  o.ok = true;
  o.status = 200;
  o.value = {str(j, "ServerName"), address, str(j, "Id")};
  return o;
}

// ---- sign-in -----------------------------------------------------------------------------------------

JfResult<bool> Jellyfin::quickConnectEnabled(const std::string& server) {
  HttpResponse r = call("GET", server, "", "/QuickConnect/Enabled");
  if (!r.ok()) return fail<bool>(r);
  JfResult<bool> o;
  o.ok = true;
  o.status = 200;
  o.value = r.body.find("true") != std::string::npos;
  return o;
}

JfResult<JfQuickConnect> Jellyfin::quickConnectStart(const std::string& server) {
  HttpResponse r = call("POST", server, "", "/QuickConnect/Initiate");
  if (!r.ok()) return fail<JfQuickConnect>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<JfQuickConnect> o;
  if (!j.is_object() || str(j, "Secret").empty()) {
    o.error = "Unexpected Quick Connect reply";
    return o;
  }
  o.ok = true;
  o.status = 200;
  o.value = {str(j, "Secret"), str(j, "Code")};
  return o;
}

JfResult<bool> Jellyfin::quickConnectPoll(const std::string& server, const std::string& secret) {
  HttpResponse r = call("GET", server, "", "/QuickConnect/Connect?secret=" + urlEncode(secret));
  if (!r.ok()) return fail<bool>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<bool> o;
  o.ok = true;
  o.status = 200;
  o.value = j.is_object() && j.value("Authenticated", false);
  return o;
}

static JfResult<JfSession> sessionFrom(const HttpResponse& r, const std::string& server) {
  JfResult<JfSession> o;
  json j = json::parse(r.body, nullptr, false);
  if (!j.is_object() || str(j, "AccessToken").empty() || !j.contains("User")) {
    o.error = "Unexpected sign-in reply";
    return o;
  }
  const json& u = j["User"];
  o.ok = true;
  o.status = 200;
  o.value.server = server;
  o.value.token = str(j, "AccessToken");
  o.value.userId = str(u, "Id");
  o.value.userName = str(u, "Name");
  return o;
}

JfResult<JfSession> Jellyfin::signInQuickConnect(const std::string& server, const std::string& secret) {
  HttpResponse r = call("POST", server, "", "/Users/AuthenticateWithQuickConnect", json{{"Secret", secret}}.dump());
  if (!r.ok()) return fail<JfSession>(r);
  return sessionFrom(r, server);
}

JfResult<JfSession> Jellyfin::signInPassword(const std::string& server, const std::string& user,
                                             const std::string& pass) {
  HttpResponse r = call("POST", server, "", "/Users/AuthenticateByName", json{{"Username", user}, {"Pw", pass}}.dump());
  if (r.status == 401) {
    JfResult<JfSession> o;
    o.status = 401;
    o.error = "Wrong username or password";
    return o;
  }
  if (!r.ok()) return fail<JfSession>(r);
  return sessionFrom(r, server);
}

// ---- items -----------------------------------------------------------------------------------------------

JfItem Jellyfin::parseItem(const void* jp) {
  const json& j = *static_cast<const json*>(jp);
  JfItem it;
  it.id = str(j, "Id");
  it.name = str(j, "Name");
  it.type = str(j, "Type");
  it.collection = str(j, "CollectionType");
  it.seriesId = str(j, "SeriesId");
  it.seriesName = str(j, "SeriesName");
  it.seasonName = str(j, "SeasonName");
  it.overview = str(j, "Overview");
  it.officialRating = str(j, "OfficialRating");
  it.year = static_cast<int>(num(j, "ProductionYear"));
  it.seasonNo = static_cast<int>(num(j, "ParentIndexNumber"));
  it.episodeNo = static_cast<int>(num(j, "IndexNumber"));
  if (it.type == "Season") it.seasonNo = static_cast<int>(num(j, "IndexNumber"));
  it.runtimeTicks = num(j, "RunTimeTicks");
  it.runtimeMin = static_cast<int>(it.runtimeTicks / 600000000LL);
  it.childCount = static_cast<int>(num(j, "ChildCount"));
  if (auto c = j.find("CommunityRating"); c != j.end() && c->is_number()) it.communityRating = c->get<double>();
  if (auto g = j.find("Genres"); g != j.end() && g->is_array())
    for (auto& e : *g)
      if (e.is_string() && it.genres.size() < 4) it.genres.push_back(e.get<std::string>());
  if (auto ud = j.find("UserData"); ud != j.end() && ud->is_object()) {
    it.positionTicks = num(*ud, "PlaybackPositionTicks");
    it.played = ud->value("Played", false);
    it.favorite = ud->value("IsFavorite", false);
    it.unplayedCount = static_cast<int>(num(*ud, "UnplayedItemCount"));
  }
  if (auto t = j.find("ImageTags"); t != j.end() && t->is_object()) {
    it.primaryTag = str(*t, "Primary");
    it.thumbTag = str(*t, "Thumb");
  }
  if (auto b = j.find("BackdropImageTags"); b != j.end() && b->is_array() && !b->empty() && (*b)[0].is_string())
    it.backdropTag = (*b)[0].get<std::string>();
  it.seriesPrimaryTag = str(j, "SeriesPrimaryImageTag");
  it.parentBackdropId = str(j, "ParentBackdropItemId");
  if (auto b = j.find("ParentBackdropImageTags"); b != j.end() && b->is_array() && !b->empty() && (*b)[0].is_string())
    it.parentBackdropTag = (*b)[0].get<std::string>();
  if (auto ms = j.find("MediaSources"); ms != j.end() && ms->is_array() && !ms->empty())
    it.mediaSourceId = str((*ms)[0], "Id");
  it.folder = j.value("IsFolder", false) || it.type == "Series" || it.type == "Season" || it.type == "BoxSet" ||
              it.type == "CollectionFolder";
  return it;
}

static std::vector<JfItem> itemsOf(const json& j) {
  std::vector<JfItem> v;
  const json* arr = nullptr;
  if (j.is_array()) arr = &j;
  else if (j.is_object() && j.contains("Items") && j["Items"].is_array()) arr = &j["Items"];
  if (arr)
    for (auto& e : *arr)
      if (e.is_object()) v.push_back(Jellyfin::parseItem(&e));
  return v;
}

namespace {
const char* kFields = "Overview,Genres,MediaSources,ProductionYear,CommunityRating,OfficialRating,ChildCount";
}

#define JF_NEED_SESSION(T)                                  \
  JfSession s = session();                                  \
  if (!s.valid()) {                                         \
    JfResult<T> e;                                          \
    e.error = "Not signed in to Jellyfin";                  \
    e.status = 401;                                         \
    return e;                                               \
  }

JfResult<std::vector<JfItem>> Jellyfin::views() {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token, "/UserViews?userId=" + s.userId);
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<std::vector<JfItem>> Jellyfin::resume(int limit) {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token,
                        "/Users/" + s.userId + "/Items/Resume?Limit=" + std::to_string(limit) +
                            "&MediaTypes=Video&Fields=" + kFields + "&EnableImageTypes=Primary,Backdrop,Thumb");
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<std::vector<JfItem>> Jellyfin::nextUp(int limit) {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token,
                        "/Shows/NextUp?userId=" + s.userId + "&Limit=" + std::to_string(limit) + "&Fields=" + kFields);
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<std::vector<JfItem>> Jellyfin::latest(const std::string& parentId, int limit) {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token,
                        "/Items/Latest?userId=" + s.userId + "&ParentId=" + urlEncode(parentId) +
                            "&Limit=" + std::to_string(limit) + "&Fields=" + kFields);
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<JfPage> Jellyfin::children(const std::string& parentId, int start, int limit, const std::string& searchTerm,
                                    bool recursive) {
  JF_NEED_SESSION(JfPage)
  std::string path = "/Items?userId=" + s.userId + "&ParentId=" + urlEncode(parentId) +
                     "&StartIndex=" + std::to_string(start) + "&Limit=" + std::to_string(limit) +
                     "&SortBy=SortName&SortOrder=Ascending&Fields=" + kFields;
  if (recursive) path += "&Recursive=true&IncludeItemTypes=Movie,Series,Episode,BoxSet";
  if (!searchTerm.empty()) path += "&SearchTerm=" + urlEncode(searchTerm);
  HttpResponse r = call("GET", s.server, s.token, path);
  if (!r.ok()) return fail<JfPage>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<JfPage> o;
  o.ok = true;
  o.status = 200;
  o.value.items = itemsOf(j);
  o.value.total = j.is_object() ? static_cast<int>(num(j, "TotalRecordCount")) : static_cast<int>(o.value.items.size());
  return o;
}

JfResult<JfPage> Jellyfin::search(const std::string& term, int limit) {
  JF_NEED_SESSION(JfPage)
  HttpResponse r = call("GET", s.server, s.token,
                        "/Items?userId=" + s.userId + "&SearchTerm=" + urlEncode(term) + "&Limit=" + std::to_string(limit) +
                            "&Recursive=true&IncludeItemTypes=Movie,Series,Episode,BoxSet&Fields=" + kFields);
  if (!r.ok()) return fail<JfPage>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<JfPage> o;
  o.ok = true;
  o.status = 200;
  o.value.items = itemsOf(j);
  o.value.total = static_cast<int>(o.value.items.size());
  return o;
}

JfResult<JfItem> Jellyfin::item(const std::string& id) {
  JF_NEED_SESSION(JfItem)
  HttpResponse r = call("GET", s.server, s.token, "/Users/" + s.userId + "/Items/" + urlEncode(id));
  if (!r.ok()) return fail<JfItem>(r);
  json j = json::parse(r.body, nullptr, false);
  JfResult<JfItem> o;
  if (!j.is_object()) {
    o.error = "Unexpected reply";
    return o;
  }
  o.ok = true;
  o.status = 200;
  o.value = parseItem(&j);
  return o;
}

JfResult<std::vector<JfItem>> Jellyfin::seasons(const std::string& seriesId) {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token, "/Shows/" + urlEncode(seriesId) + "/Seasons?userId=" + s.userId);
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<std::vector<JfItem>> Jellyfin::episodes(const std::string& seriesId, const std::string& seasonId) {
  JF_NEED_SESSION(std::vector<JfItem>)
  HttpResponse r = call("GET", s.server, s.token,
                        "/Shows/" + urlEncode(seriesId) + "/Episodes?userId=" + s.userId + "&SeasonId=" + urlEncode(seasonId) +
                            "&Fields=" + kFields);
  if (!r.ok()) return fail<std::vector<JfItem>>(r);
  JfResult<std::vector<JfItem>> o;
  o.ok = true;
  o.status = 200;
  o.value = itemsOf(json::parse(r.body, nullptr, false));
  return o;
}

JfResult<bool> Jellyfin::setPlayed(const std::string& id, bool played) {
  JF_NEED_SESSION(bool)
  HttpResponse r = call(played ? "POST" : "DELETE", s.server, s.token,
                        "/Users/" + s.userId + "/PlayedItems/" + urlEncode(id));
  if (!r.ok()) return fail<bool>(r);
  JfResult<bool> o;
  o.ok = true;
  o.status = 200;
  o.value = played;
  return o;
}

std::string Jellyfin::imageUrl(const std::string& id, const std::string& kind, const std::string& tag, int maxHeight) const {
  JfSession s = session();
  if (s.server.empty() || id.empty()) return {};
  std::string u = s.server + "/Items/" + urlEncode(id) + "/Images/" + kind + "?maxHeight=" + std::to_string(maxHeight) + "&quality=85";
  if (!tag.empty()) u += "&tag=" + urlEncode(tag);
  return u;
}

std::string Jellyfin::streamUrl(const JfItem& it) const {
  JfSession s = session();
  std::string u = s.server + "/Videos/" + urlEncode(it.id) + "/stream?static=true";
  if (!it.mediaSourceId.empty()) u += "&MediaSourceId=" + urlEncode(it.mediaSourceId);
  return u;
}

// ---- playback reporting -------------------------------------------------------------------------------------

static std::string playBody(const JfItem& it, int64_t ticks, bool paused, const std::string& playSession) {
  json b = {{"ItemId", it.id},
            {"PositionTicks", ticks},
            {"IsPaused", paused},
            {"PlayMethod", "DirectPlay"},
            {"PlaySessionId", playSession},
            {"CanSeek", true}};
  if (!it.mediaSourceId.empty()) b["MediaSourceId"] = it.mediaSourceId;
  return b.dump();
}

void Jellyfin::reportStart(const JfItem& it, int64_t ticks, const std::string& ps) {
  JfSession s = session();
  if (!s.valid()) return;
  HttpResponse r = call("POST", s.server, s.token, "/Sessions/Playing", playBody(it, ticks, false, ps));
  if (!r.ok()) logf("jellyfin: report start failed (%ld)", r.status);
}
void Jellyfin::reportProgress(const JfItem& it, int64_t ticks, bool paused, const std::string& ps) {
  JfSession s = session();
  if (!s.valid()) return;
  HttpResponse r = call("POST", s.server, s.token, "/Sessions/Playing/Progress", playBody(it, ticks, paused, ps));
  if (!r.ok()) logf("jellyfin: report progress failed (%ld)", r.status);
}
void Jellyfin::reportStopped(const JfItem& it, int64_t ticks, const std::string& ps) {
  JfSession s = session();
  if (!s.valid()) return;
  HttpResponse r = call("POST", s.server, s.token, "/Sessions/Playing/Stopped", playBody(it, ticks, false, ps));
  if (!r.ok()) logf("jellyfin: report stopped failed (%ld)", r.status);
}

}  // namespace vos
