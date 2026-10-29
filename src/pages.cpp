#include "pages.hpp"

#include <algorithm>
#include <cmath>

#include "json.hpp"
#include "widgets.hpp"

namespace vos {

using nlohmann::json;

namespace {

constexpr float kMargin = 96.f;
constexpr float kTop = 132.f;      // below the top bar
constexpr float kBottom = 1000.f;  // above the hint bar

TextStyle mono(float size, Color c, float tracking = 0) {
  TextStyle t;
  t.font = Font::Mono;
  t.size = size;
  t.color = c;
  t.tracking = tracking;
  return t;
}
TextStyle head(float size, Color c, float tracking = 0, bool bold = true) {
  TextStyle t;
  t.font = Font::Head;
  t.size = size;
  t.color = c;
  t.tracking = tracking;
  t.bold = bold;
  return t;
}

std::string jstr(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
double jnum(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number()) ? it->get<double>() : 0;
}
bool jbool(const json& j, const char* k, bool d) {
  auto it = j.find(k);
  return (it != j.end() && it->is_boolean()) ? it->get<bool>() : d;
}

// "2026-10-28T20:11:00.000Z" -> "28 OCT 2026"
std::string fmtDate(const std::string& iso) {
  if (iso.size() < 10) return "";
  static const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  int m = std::atoi(iso.substr(5, 2).c_str());
  if (m < 1 || m > 12) return "";
  return std::to_string(std::atoi(iso.substr(8, 2).c_str())) + " " + months[m - 1] + " " + iso.substr(0, 4);
}

// First row to draw so that `sel` stays inside `avail`.
int windowStart(const std::vector<float>& h, int sel, float avail) {
  int start = 0;
  for (;; ++start) {
    float sum = 0;
    for (int i = start; i <= sel && i < static_cast<int>(h.size()); ++i) sum += h[i];
    if (sum <= avail || start >= sel) break;
  }
  return start;
}

void panel(Gfx& g, float x, float y, float w, float h, bool focus) {
  g.rrect(x, y, w, h, 10, Color(14, 11, 10, 215));
  g.rrectLine(x, y, w, h, 10, focus ? 2.f : 1.2f, focus ? theme::magma.alpha(0.85f) : theme::line);
}

void rowBg(Gfx& g, float x, float y, float w, float h, bool sel, bool focus) {
  if (!sel) return;
  g.hgrad(x, y, w, h, theme::magma.alpha(focus ? 0.30f : 0.14f), theme::magma.alpha(0.02f));
  g.rect(x, y, 4, h, focus ? theme::magma : theme::dim);
}

void toggle(Gfx& g, float x, float y, bool on) {
  float w = 64, h = 32;
  g.rrect(x, y, w, h, h / 2, on ? Color(255, 106, 31, 255) : theme::panelHi);
  g.rrectLine(x, y, w, h, h / 2, 1.4f, on ? Color(255, 178, 71) : theme::line);
  g.circle(x + (on ? w - h / 2 : h / 2), y + h / 2, h / 2 - 5, on ? Color(20, 10, 6) : theme::dim);
}

void medal(Gfx& g, float cx, float cy, float r, bool unlocked, double now) {
  if (unlocked) {
    g.glow(cx, cy, r * 2.2f, r * 2.2f, theme::ember.alpha(0.25f));
    g.circle(cx, cy, r, Color(64, 38, 18));
    g.ring(cx, cy, r, 3, theme::ember);
    g.line(cx - r * 0.38f, cy + 1, cx - r * 0.08f, cy + r * 0.34f, 3, theme::ember);
    g.line(cx - r * 0.08f, cy + r * 0.34f, cx + r * 0.42f, cy - r * 0.32f, 3, theme::ember);
  } else {
    g.circle(cx, cy, r, theme::panel);
    g.ring(cx, cy, r, 2, theme::line);
    g.rrect(cx - r * 0.28f, cy - r * 0.05f, r * 0.56f, r * 0.42f, 3, theme::faint);
    g.arc(cx, cy - r * 0.08f, r * 0.26f, 3, 3.14159f, 6.28318f, theme::faint);
  }
  (void)now;
}

}  // namespace

const char* Pages::label(int i) {
  static const char* l[kTabs] = {"ACHIEVEMENTS", "SOCIAL", "PROFILE"};
  return l[i];
}

// ---- loading --------------------------------------------------------------------

void Pages::fetch(const std::string& path, std::function<void(const std::string&)> ok, bool quiet) {
  if (demo_) return;
  pool_.run<Result<std::string>>(
      main_, [this, path] { return api_.request("GET", path); },
      [this, ok, quiet](Result<std::string> r) {
        if (r.ok) ok(r.value);
        else if (r.unauthorized()) h_.unauthorized();
        else if (!quiet) h_.toast(upper(r.error), theme::err);
      });
}

void Pages::reset() {
  achLoaded_ = socLoaded_ = proLoaded_ = false;
  groups_.clear(); friends_.clear(); reqs_.clear(); acts_.clear(); devices_.clear(); playtime_.clear();
  chat_.clear(); chatFor_ = 0;
  lastLoad_[0] = lastLoad_[1] = lastLoad_[2] = 0;
}

void Pages::open(PageTab t) {
  tab_ = t;
  double now = nowSeconds();
  int i = static_cast<int>(t);
  if (demo_) return;
  // Reload when stale so a page always opens fresh but never blanks.
  if (now - lastLoad_[i] > 20) {
    lastLoad_[i] = now;
    if (t == PageTab::Achievements) loadAchievements();
    else if (t == PageTab::Social) loadSocial();
    else loadProfile();
  }
}

void Pages::loadAchievements() {
  achLoading_ = true;
  pool_.run<Result<std::string>>(
      main_, [this] { return api_.request("GET", "/api/achievements"); },
      [this](Result<std::string> r) {
        achLoading_ = false;
        if (!r.ok) {
          if (r.unauthorized()) h_.unauthorized();
          else h_.toast(upper(r.error), theme::err);
          return;
        }
        json j = json::parse(r.value, nullptr, false);
        if (!j.is_object()) return;
        groups_.clear();
        if (j.contains("stats") && j["stats"].is_object()) {
          achUnlocked_ = static_cast<int>(jnum(j["stats"], "unlockedTotal"));
          achTotal_ = static_cast<int>(jnum(j["stats"], "total"));
          achPerfect_ = static_cast<int>(jnum(j["stats"], "perfect"));
        }
        if (j.contains("groups") && j["groups"].is_array()) {
          for (auto& gr : j["groups"]) {
            AchGroup grp;
            if (gr.contains("game") && gr["game"].is_object()) grp.name = jstr(gr["game"], "name");
            grp.unlocked = static_cast<int>(jnum(gr, "unlocked"));
            grp.total = static_cast<int>(jnum(gr, "total"));
            if (gr.contains("items") && gr["items"].is_array())
              for (auto& it : gr["items"])
                grp.items.push_back({jstr(it, "name"), jstr(it, "description"), jstr(it, "unlockedAt"), jnum(it, "rarityPct")});
            groups_.push_back(std::move(grp));
          }
        }
        achLoaded_ = true;
        gSel_ = std::min(gSel_, std::max(0, static_cast<int>(groups_.size()) - 1));
      });
}

void Pages::loadSocial() {
  socLoading_ = true;
  struct Bundle { Result<std::string> f, r, a; };
  pool_.run<Bundle>(
      main_,
      [this] {
        return Bundle{api_.request("GET", "/api/friends"), api_.request("GET", "/api/friends/requests"),
                      api_.request("GET", "/api/activity?limit=12")};
      },
      [this](Bundle b) {
        socLoading_ = false;
        if (b.f.unauthorized()) return h_.unauthorized();
        if (!b.f.ok) return h_.toast(upper(b.f.error), theme::err);
        friends_.clear(); reqs_.clear(); acts_.clear();
        json f = json::parse(b.f.value, nullptr, false);
        if (f.is_array())
          for (auto& e : f) friends_.push_back({static_cast<int64_t>(jnum(e, "id")), jstr(e, "gamertag"), jstr(e, "status"), jbool(e, "online", false)});
        if (b.r.ok) {
          json r = json::parse(b.r.value, nullptr, false);
          if (r.is_array()) for (auto& e : r) reqs_.push_back({static_cast<int64_t>(jnum(e, "id")), jstr(e, "gamertag")});
        }
        if (b.a.ok) {
          json a = json::parse(b.a.value, nullptr, false);
          if (a.is_array()) for (auto& e : a) acts_.push_back({jstr(e, "gamertag"), jstr(e, "detail"), jstr(e, "createdAt")});
        }
        socLoaded_ = true;
        rebuildSocialRows();
      });
}

void Pages::loadChat(int64_t id) {
  chatFor_ = id;
  chatFetched_ = nowSeconds();
  if (demo_) return;
  pool_.run<Result<std::string>>(
      main_, [this, id] { return api_.request("GET", "/api/messages/" + std::to_string(id)); },
      [this, id](Result<std::string> r) {
        if (!r.ok || chatFor_ != id) return;
        json j = json::parse(r.value, nullptr, false);
        if (!j.is_array()) return;
        chat_.clear();
        for (auto& m : j) chat_.push_back({jbool(m, "mine", false), jstr(m, "body")});
      });
}

void Pages::loadProfile() {
  proLoading_ = true;
  struct Bundle { Result<std::string> p, d, s; };
  pool_.run<Bundle>(
      main_,
      [this] {
        return Bundle{api_.request("GET", "/api/profile"), api_.request("GET", "/api/devices"), api_.request("GET", "/api/settings")};
      },
      [this](Bundle b) {
        proLoading_ = false;
        if (b.p.unauthorized()) return h_.unauthorized();
        if (!b.p.ok) return h_.toast(upper(b.p.error), theme::err);
        json p = json::parse(b.p.value, nullptr, false);
        if (p.is_object()) {
          gamertag_ = jstr(p, "gamertag");
          bio_ = jstr(p, "bio");
          memberSince_ = jstr(p, "memberSince");
          level_ = static_cast<int>(jnum(p, "level"));
          xp_ = static_cast<int>(jnum(p, "xpIntoLevel"));
          if (p.contains("stats") && p["stats"].is_object()) {
            titles_ = static_cast<int>(jnum(p["stats"], "titlesOwned"));
            hours_ = jnum(p["stats"], "hoursPlayed");
            achCount_ = static_cast<int>(jnum(p["stats"], "achievements"));
            friendCount_ = static_cast<int>(jnum(p["stats"], "friends"));
          }
          playtime_.clear();
          if (p.contains("playtime") && p["playtime"].is_array())
            for (auto& e : p["playtime"]) playtime_.emplace_back(jstr(e, "name"), static_cast<int>(jnum(e, "minutes")));
        }
        devices_.clear();
        if (b.d.ok) {
          json d = json::parse(b.d.value, nullptr, false);
          if (d.is_object() && d.contains("devices") && d["devices"].is_array())
            for (auto& e : d["devices"]) {
              Device dv;
              dv.id = static_cast<int64_t>(jnum(e, "id"));
              dv.name = jstr(e, "name");
              dv.platform = jstr(e, "platform") + "/" + jstr(e, "arch");
              dv.lastSeen = jstr(e, "lastSeenAt");
              if (e.contains("installs") && e["installs"].is_array())
                for (auto& i : e["installs"]) dv.installs.push_back(jstr(i, "name"));
              devices_.push_back(std::move(dv));
            }
        }
        if (b.s.ok) {
          json s = json::parse(b.s.value, nullptr, false);
          if (s.is_object()) {
            const char* keys[5] = {"showActivity", "allowFriendRequests", "chatNonFriends", "cloudSaves", "patchEmails"};
            for (int i = 0; i < 5; ++i) settings_[i] = jbool(s, keys[i], settings_[i]);
          }
        }
        proLoaded_ = true;
        rebuildProfileRows();
      });
}

void Pages::rebuildSocialRows() {
  srows_.clear();
  srows_.push_back({SocialRow::Add, 0});
  if (!reqs_.empty()) {
    srows_.push_back({SocialRow::Header, 0});
    for (size_t i = 0; i < reqs_.size(); ++i) srows_.push_back({SocialRow::Request, static_cast<int>(i)});
  }
  if (!friends_.empty()) {
    srows_.push_back({SocialRow::Header, 1});
    for (size_t i = 0; i < friends_.size(); ++i) srows_.push_back({SocialRow::Friend, static_cast<int>(i)});
  }
  sSel_ = std::min(sSel_, static_cast<int>(srows_.size()) - 1);
  if (srows_[sSel_].kind == SocialRow::Header) sSel_ = 0;
}

void Pages::rebuildProfileRows() {
  prows_.clear();
  prows_.push_back({ProfileRow::Bio, 0});
  prows_.push_back({ProfileRow::Header, 0});
  for (int i = 0; i < 5; ++i) prows_.push_back({ProfileRow::Setting, i});
  if (!devices_.empty()) {
    prows_.push_back({ProfileRow::Header, 1});
    for (size_t i = 0; i < devices_.size(); ++i) prows_.push_back({ProfileRow::Device, static_cast<int>(i)});
  }
  pSel_ = std::min(pSel_, static_cast<int>(prows_.size()) - 1);
  if (prows_[pSel_].kind == ProfileRow::Header) pSel_ = 0;
}

void Pages::loadDemo() {
  demo_ = true;
  achLoaded_ = socLoaded_ = proLoaded_ = true;
  achUnlocked_ = 26; achTotal_ = 77; achPerfect_ = 1;
  groups_ = {
      {"PEGGY!", 12, 12, {{"First Bounce", "Launch your first ball.", "2026-09-02T18:00:00Z", 94.2}, {"Button Hoarder", "Collect 500 buttons in a single run.", "2026-09-14T20:10:00Z", 41.0}, {"Hedgehog Hustle", "Clear a board without touching a bumper.", "2026-09-30T09:12:00Z", 12.5}}},
      {"CARVE THE FATE", 7, 24, {{"Dead Air", "Survive the first tide.", "2026-10-02T21:00:00Z", 71.3}, {"Forgemaster", "Forge a blade with three runes.", "2026-10-11T19:40:00Z", 33.8}, {"Tidebreaker", "Defeat the Drowned King.", "", 6.1}, {"Cartographer", "Reveal 90% of the map.", "", 3.4}}},
      {"CYCIC CITIZEN", 2, 18, {{"First Lane", "Build your first bike lane.", "2026-10-20T10:00:00Z", 88.0}, {"Smug Commuter", "Reach 1,000 daily cyclists.", "", 9.9}}},
      {"PINE", 1, 6, {{"Into The Woods", "Take your first step.", "2026-10-01T22:50:00Z", 97.0}}},
  };
  friends_ = {{1, "Marlowe", "Playing Peggy!", true}, {2, "ash_wednesday", "Online", true}, {3, "Quill", "Last seen 3h ago", false}, {4, "dr.dumpling", "Last seen 2d ago", false}};
  reqs_ = {{10, "TheBigCheese"}};
  acts_ = {{"Marlowe", "unlocked Hedgehog Hustle in Peggy!", "2026-10-28T20:11:00Z"}, {"Quill", "reached level 9", "2026-10-28T14:00:00Z"}, {"ash_wednesday", "started playing Fruit Heist", "2026-10-27T22:05:00Z"}};
  chatFor_ = 1;
  chat_ = {{false, "you around for a round of Fruit Heist later?"}, {true, "yeah, after tea. 8ish?"}, {false, "perfect. I'll bring the getaway pears."}};
  rebuildSocialRows();
  gamertag_ = "Nikorag"; bio_ = "Making small games with large feelings."; memberSince_ = "2025-03-14T00:00:00Z";
  level_ = 14; xp_ = 2600; titles_ = 8; hours_ = 43.5; achCount_ = 26; friendCount_ = 4;
  playtime_ = {{"Carve The Fate", 1830}, {"Peggy!", 640}, {"Hungry Birds", 210}, {"Cycic Citizen", 95}, {"Pine", 45}};
  devices_ = {{1, "Nikorag-MBP", "darwin/aarch64", "2026-10-29T12:00:00Z", {"Carve The Fate", "Peggy!", "Pine"}}, {2, "Living room PC", "windows/x86_64", "2026-10-12T09:00:00Z", {"Peggy!"}}};
  rebuildProfileRows();
}

// ---- input -------------------------------------------------------------------------

bool Pages::handle(Action a) {
  if (a == Action::LB || a == Action::RB) {
    int t = (static_cast<int>(tab_) + (a == Action::RB ? 1 : kTabs - 1)) % kTabs;
    open(static_cast<PageTab>(t));
    h_.tick();
    return false;
  }
  if (a == Action::Back || a == Action::Home || a == Action::Start) return true;
  switch (tab_) {
    case PageTab::Achievements: return handleAch(a);
    case PageTab::Social: return handleSocial(a);
    case PageTab::Profile: return handleProfile(a);
  }
  return false;
}

bool Pages::handleAch(Action a) {
  int ng = static_cast<int>(groups_.size());
  if (a == Action::Left) inItems_ = false;
  else if (a == Action::Right && ng) inItems_ = true;
  else if (a == Action::Up || a == Action::Down) {
    int d = a == Action::Down ? 1 : -1;
    if (!inItems_) {
      int ns = std::max(0, std::min(ng - 1, gSel_ + d));
      if (ns != gSel_) { gSel_ = ns; aSel_ = 0; h_.tick(); }
    } else if (ng) {
      int n = static_cast<int>(groups_[gSel_].items.size());
      aSel_ = std::max(0, std::min(n - 1, aSel_ + d));
    }
  } else if (a == Action::Y) {
    achLoaded_ ? loadAchievements() : void();
  }
  return false;
}

bool Pages::handleSocial(Action a) {
  int n = static_cast<int>(srows_.size());
  if (a == Action::Up || a == Action::Down) {
    int d = a == Action::Down ? 1 : -1;
    int s = sSel_;
    do { s += d; } while (s >= 0 && s < n && srows_[s].kind == SocialRow::Header);
    if (s >= 0 && s < n) { sSel_ = s; h_.tick(); }
    return false;
  }
  if (a == Action::Y) {
    loadSocial();
    return false;
  }
  if (srows_.empty()) return false;
  const SocialRow& row = srows_[sSel_];
  if (row.kind == SocialRow::Add && a == Action::Accept) {
    h_.text("ADD A FRIEND", "", 24, [this](const std::string& tag) {
      if (tag.empty()) return;
      if (demo_) return h_.toast("REQUEST SENT TO " + upper(tag), theme::ok);
      pool_.run<Result<std::string>>(
          main_, [this, tag] { return api_.request("POST", "/api/friends", json{{"gamertag", tag}}.dump()); },
          [this, tag](Result<std::string> r) {
            if (r.ok) h_.toast("REQUEST SENT TO " + upper(tag), theme::ok);
            else h_.toast(upper(r.error), theme::err);
          });
    });
  } else if (row.kind == SocialRow::Request && (a == Action::Accept || a == Action::X)) {
    Req rq = reqs_[row.idx];
    bool accept = a == Action::Accept;
    if (demo_) {
      reqs_.erase(reqs_.begin() + row.idx);
      rebuildSocialRows();
      return false;
    }
    pool_.run<Result<std::string>>(
        main_, [this, rq, accept] { return api_.request(accept ? "POST" : "DELETE", "/api/friends/requests/" + std::to_string(rq.id)); },
        [this, rq, accept](Result<std::string> r) {
          if (!r.ok) return h_.toast(upper(r.error), theme::err);
          h_.toast(accept ? "YOU AND " + upper(rq.gamertag) + " ARE NOW FRIENDS" : "REQUEST DECLINED", accept ? theme::ok : theme::dim);
          loadSocial();
        });
  } else if (row.kind == SocialRow::Friend && a == Action::Accept) {
    Friend f = friends_[row.idx];
    h_.text("MESSAGE " + upper(f.gamertag), "", 280, [this, f](const std::string& body) {
      if (body.empty()) return;
      if (demo_) {
        chat_.push_back({true, body});
        return;
      }
      pool_.run<Result<std::string>>(
          main_, [this, f, body] { return api_.request("POST", "/api/messages/" + std::to_string(f.id), json{{"body", body}}.dump()); },
          [this, f](Result<std::string> r) {
            if (!r.ok) return h_.toast(upper(r.error), theme::err);
            loadChat(f.id);
          });
    });
  }
  return false;
}

bool Pages::handleProfile(Action a) {
  int n = static_cast<int>(prows_.size());
  if (a == Action::Up || a == Action::Down) {
    int d = a == Action::Down ? 1 : -1;
    int s = pSel_;
    do { s += d; } while (s >= 0 && s < n && prows_[s].kind == ProfileRow::Header);
    if (s >= 0 && s < n) { pSel_ = s; h_.tick(); }
    return false;
  }
  if (a == Action::Y) {
    loadProfile();
    return false;
  }
  if (prows_.empty()) return false;
  const ProfileRow row = prows_[pSel_];
  if (row.kind == ProfileRow::Bio && a == Action::Accept) {
    h_.text("EDIT YOUR BIO", bio_, 280, [this](const std::string& bio) {
      std::string old = bio_;
      bio_ = bio;
      if (demo_) return;
      pool_.run<Result<std::string>>(
          main_, [this, bio] { return api_.request("PATCH", "/api/profile", json{{"bio", bio}}.dump()); },
          [this, old](Result<std::string> r) {
            if (!r.ok) {
              bio_ = old;
              h_.toast(upper(r.error), theme::err);
            }
          });
    });
  } else if (row.kind == ProfileRow::Setting && a == Action::Accept) {
    static const char* keys[5] = {"showActivity", "allowFriendRequests", "chatNonFriends", "cloudSaves", "patchEmails"};
    int i = row.idx;
    bool nv = !settings_[i];
    settings_[i] = nv;  // optimistic
    h_.tick();
    if (demo_) return false;
    pool_.run<Result<std::string>>(
        main_, [this, i, nv] { return api_.request("PATCH", "/api/settings", json{{keys[i], nv}}.dump()); },
        [this, i, nv](Result<std::string> r) {
          if (!r.ok) {
            settings_[i] = !nv;
            h_.toast(upper(r.error), theme::err);
          }
        });
  } else if (row.kind == ProfileRow::Device && a == Action::X) {
    Device d = devices_[row.idx];
    h_.confirm("FORGET " + upper(d.name) + "?", "Removes this device and its install records from your account. It reappears if its launcher is still in use.", "FORGET DEVICE",
               [this, d] {
                 if (demo_) {
                   devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [&](const Device& x) { return x.id == d.id; }), devices_.end());
                   rebuildProfileRows();
                   return;
                 }
                 pool_.run<Result<std::string>>(
                     main_, [this, d] { return api_.request("DELETE", "/api/devices/" + std::to_string(d.id)); },
                     [this, d](Result<std::string> r) {
                       if (!r.ok) return h_.toast(upper(r.error), theme::err);
                       h_.toast("FORGOT " + upper(d.name), theme::dim);
                       loadProfile();
                     });
               });
  }
  return false;
}

// ---- drawing -------------------------------------------------------------------------

void Pages::hints(PadStyle st, const std::vector<std::pair<Action, std::string>>& items) {
  float x = kMargin, y = 1026;
  for (auto& [a, l] : items) x += ui::prompt(g_, st, a, l, x, y, 22) + 44;
  x += ui::prompt(g_, st, Action::LB, "", x, y, 22) + 8;
  x += ui::prompt(g_, st, Action::RB, "TABS", x, y, 22) + 44;
  float w = ui::promptWidth(g_, st, Action::Back, "BACK", 22);
  ui::prompt(g_, st, Action::Back, "BACK", g_.W() - kMargin - w, y, 22);
}

void Pages::drawEmpty(const std::string& title, const std::string& sub, bool loading, double now) {
  float cx = g_.W() / 2, cy = 480;
  if (loading) g_.arc(cx, cy, 24, 4, static_cast<float>(now) * 4, static_cast<float>(now) * 4 + 4.2f, theme::magma);
  TextStyle t = head(34, theme::dim, 5);
  t.align = Align::Center;
  g_.text(title, cx, cy + 50, t);
  TextStyle s = mono(16, theme::faint, 2);
  s.align = Align::Center;
  g_.text(sub, cx, cy + 108, s);
}

void Pages::draw(double now, float dt, PadStyle st) {
  (void)dt;
  switch (tab_) {
    case PageTab::Achievements: drawAch(now, st); break;
    case PageTab::Social: drawSocial(now, st); break;
    case PageTab::Profile: drawProfile(now, st); break;
  }
}

void Pages::drawAch(double now, PadStyle st) {
  const float W = g_.W();
  if (!achLoaded_) {
    drawEmpty(achLoading_ ? "LOADING ACHIEVEMENTS" : "NOTHING TO SHOW", "", achLoading_, now);
    hints(st, {});
    return;
  }
  // Summary strip.
  float y = kTop + 8;
  struct S { std::string l, v; Color c; };
  std::vector<S> sum = {{"UNLOCKED", std::to_string(achUnlocked_) + " / " + std::to_string(achTotal_), theme::ember},
                        {"COMPLETED TITLES", std::to_string(achPerfect_), theme::text},
                        {"COMPLETION", achTotal_ ? std::to_string(achUnlocked_ * 100 / achTotal_) + "%" : "0%", theme::text}};
  float sx = kMargin;
  for (auto& s : sum) {
    g_.text(s.l, sx, y, mono(13, theme::faint, 3.5f));
    float w = g_.text(s.v, sx, y + 22, head(40, s.c, 1)).x;
    sx += std::max(w, 160.f) + 60;
  }
  ui::progressBar(g_, W - kMargin - 420, y + 34, 420, 8, achTotal_ ? static_cast<float>(achUnlocked_) / achTotal_ : 0, theme::ember);

  float top = y + 92, bottom = kBottom;
  float lw = 560, rx = kMargin + lw + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, lw, bottom - top, !inItems_);
  panel(g_, rx, top, rw, bottom - top, inItems_);
  if (groups_.empty()) {
    TextStyle t = head(26, theme::dim, 4);
    g_.text("NO ACHIEVEMENTS YET", kMargin + 36, top + 40, t);
    hints(st, {});
    return;
  }

  // Left: games.
  const float rh = 88;
  int rows = static_cast<int>((bottom - top - 24) / rh);
  int first = std::max(0, std::min(gSel_ - rows / 2, static_cast<int>(groups_.size()) - rows));
  for (int i = first; i < static_cast<int>(groups_.size()) && i < first + rows; ++i) {
    const AchGroup& gr = groups_[i];
    float ry = top + 12 + (i - first) * rh;
    rowBg(g_, kMargin + 10, ry, lw - 20, rh - 8, i == gSel_, !inItems_);
    TextStyle n = head(22, i == gSel_ ? theme::text : theme::dim, 2.5f);
    g_.text(g_.ellipsize(gr.name, n, lw - 190), kMargin + 36, ry + 10, n);
    bool done = gr.total > 0 && gr.unlocked == gr.total;
    TextStyle c = mono(16, done ? theme::ok : theme::dim, 2);
    c.align = Align::Right;
    g_.text(std::to_string(gr.unlocked) + " / " + std::to_string(gr.total), kMargin + lw - 30, ry + 14, c);
    ui::progressBar(g_, kMargin + 36, ry + 50, lw - 72, 6, gr.total ? static_cast<float>(gr.unlocked) / gr.total : 0, done ? theme::ok : theme::ember);
  }

  // Right: achievements of the selected game.
  const AchGroup& gr = groups_[gSel_];
  TextStyle ht = head(30, theme::text, 4);
  g_.text(g_.ellipsize(gr.name, ht, rw - 80), rx + 36, top + 26, ht);
  g_.rect(rx + 36, top + 82, rw - 72, 1, theme::line);
  const float ah = 100;
  float avail = bottom - (top + 96) - 12;
  int arows = std::max(1, static_cast<int>(avail / ah));
  int n = static_cast<int>(gr.items.size());
  aSel_ = std::max(0, std::min(aSel_, n - 1));
  int afirst = std::max(0, std::min(aSel_ - arows / 2, n - arows));
  for (int i = afirst; i < n && i < afirst + arows; ++i) {
    const Ach& a = gr.items[i];
    bool got = !a.unlockedAt.empty();
    float ry = top + 96 + (i - afirst) * ah;
    rowBg(g_, rx + 10, ry, rw - 20, ah - 8, inItems_ && i == aSel_, true);
    medal(g_, rx + 64, ry + (ah - 8) / 2, 26, got, now);
    TextStyle n1 = head(22, got ? theme::text : theme::dim, 2);
    g_.text(g_.ellipsize(upper(a.name), n1, rw - 420), rx + 112, ry + 12, n1);
    TextStyle d1 = head(18, theme::faint, 0, false);
    g_.text(g_.ellipsize(a.desc, d1, rw - 420), rx + 112, ry + 46, d1);
    TextStyle r1 = mono(14, got ? theme::ember : theme::faint, 2);
    r1.align = Align::Right;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.1f%% OF PLAYERS", a.rarity);
    g_.text(buf, rx + rw - 36, ry + 18, r1);
    TextStyle u1 = mono(13, got ? theme::ok : theme::faint, 2);
    u1.align = Align::Right;
    g_.text(got ? "UNLOCKED " + fmtDate(a.unlockedAt) : "LOCKED", rx + rw - 36, ry + 50, u1);
  }
  if (n > arows) {
    g_.text(std::to_string(aSel_ + 1) + " / " + std::to_string(n), rx + rw - 36, top + 36, [] { auto t = mono(14, theme::faint, 2); t.align = Align::Right; return t; }());
  }
  hints(st, {{Action::Left, ""}, {Action::Y, "REFRESH"}});
}

void Pages::drawSocial(double now, PadStyle st) {
  const float W = g_.W();
  if (!socLoaded_) {
    drawEmpty(socLoading_ ? "LOADING SOCIAL" : "NOTHING TO SHOW", "", socLoading_, now);
    hints(st, {});
    return;
  }
  float top = kTop + 8, bottom = kBottom, lw = 560, rx = kMargin + lw + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, lw, bottom - top, true);
  panel(g_, rx, top, rw, bottom - top, false);

  // Left list with variable-height rows.
  std::vector<float> hs;
  for (auto& r : srows_) hs.push_back(r.kind == SocialRow::Header ? 46.f : 78.f);
  int first = windowStart(hs, sSel_, bottom - top - 24);
  float y = top + 12;
  for (int i = first; i < static_cast<int>(srows_.size()); ++i) {
    if (y + hs[i] > bottom - 8) break;
    const SocialRow& r = srows_[i];
    bool on = i == sSel_;
    if (r.kind == SocialRow::Header) {
      g_.text(r.idx == 0 ? "FRIEND REQUESTS  " + std::to_string(reqs_.size()) : "FRIENDS  " + std::to_string(friends_.size()), kMargin + 36, y + 18, mono(13, theme::faint, 4));
    } else {
      rowBg(g_, kMargin + 10, y, lw - 20, hs[i] - 6, on, true);
      if (r.kind == SocialRow::Add) {
        g_.circle(kMargin + 62, y + 36, 22, theme::panelHi);
        g_.line(kMargin + 62 - 9, y + 36, kMargin + 62 + 9, y + 36, 3, theme::magma);
        g_.line(kMargin + 62, y + 36 - 9, kMargin + 62, y + 36 + 9, 3, theme::magma);
        g_.text("ADD FRIEND", kMargin + 104, y + 24, head(22, on ? theme::text : theme::dim, 3));
      } else {
        const std::string& tag = r.kind == SocialRow::Request ? reqs_[r.idx].gamertag : friends_[r.idx].gamertag;
        bool online = r.kind == SocialRow::Friend && friends_[r.idx].online;
        g_.circle(kMargin + 62, y + 36, 22, theme::panelHi);
        g_.ring(kMargin + 62, y + 36, 22, 2, online ? theme::ok : theme::line);
        TextStyle in = head(22, online ? theme::ok : theme::dim, 0);
        in.align = Align::Center;
        std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(tag.empty() ? '?' : tag[0]))));
        g_.text(ini, kMargin + 62, y + 36 - g_.measure(ini, in).y / 2, in);
        g_.text(g_.ellipsize(upper(tag), head(22, theme::text, 2), lw - 190), kMargin + 104, y + 14, head(22, on ? theme::text : theme::dim, 2));
        std::string sub = r.kind == SocialRow::Request ? "WANTS TO BE FRIENDS" : upper(friends_[r.idx].status);
        g_.text(g_.ellipsize(sub, mono(13, theme::faint, 1.5f), lw - 190), kMargin + 104, y + 46, mono(13, online ? theme::ok : theme::faint, 1.5f));
      }
    }
    y += hs[i];
  }

  // Right pane by selection.
  const SocialRow& sel = srows_[sSel_];
  if (sel.kind == SocialRow::Add) {
    g_.text("FRIEND ACTIVITY", rx + 40, top + 30, head(28, theme::text, 5));
    g_.rect(rx + 40, top + 82, rw - 80, 1, theme::line);
    if (acts_.empty()) g_.text("Nothing from your friends yet.", rx + 40, top + 110, mono(16, theme::faint, 1));
    float ay = top + 104;
    for (size_t i = 0; i < acts_.size() && ay < bottom - 80; ++i) {
      g_.circle(rx + 52, ay + 20, 5, theme::magma);
      TextStyle a = head(22, theme::text, 1);
      float w = g_.text(upper(acts_[i].gamertag), rx + 76, ay + 4, a).x;
      g_.text(g_.ellipsize(acts_[i].detail, head(20, theme::dim, 0, false), rw - 140 - w), rx + 76 + w + 14, ay + 6, head(20, theme::dim, 0, false));
      g_.text(fmtDate(acts_[i].createdAt), rx + 76, ay + 36, mono(12, theme::faint, 2));
      ay += 78;
    }
    hints(st, {{Action::Accept, "ADD FRIEND"}, {Action::Y, "REFRESH"}});
  } else if (sel.kind == SocialRow::Request) {
    const Req& rq = reqs_[sel.idx];
    TextStyle t = head(54, theme::text, 3);
    g_.text(g_.ellipsize(upper(rq.gamertag), t, rw - 80), rx + 40, top + 60, t);
    g_.text("WANTS TO BE FRIENDS WITH YOU", rx + 40, top + 140, mono(16, theme::magma, 4));
    ui::prompt(g_, st, Action::Accept, "ACCEPT", rx + 40, top + 220, 28);
    ui::prompt(g_, st, Action::X, "DECLINE", rx + 40, top + 280, 28);
    hints(st, {{Action::Accept, "ACCEPT"}, {Action::X, "DECLINE"}});
  } else if (sel.kind == SocialRow::Friend) {
    const Friend& f = friends_[sel.idx];
    if (chatFor_ != f.id) {
      chat_.clear();
      loadChat(f.id);
    } else if (!demo_ && now - chatFetched_ > 8) {
      loadChat(f.id);
    }
    g_.circle(rx + 52, top + 50, 8, f.online ? theme::ok : theme::faint);
    g_.text(g_.ellipsize(upper(f.gamertag), head(30, theme::text, 4), rw - 160), rx + 76, top + 30, head(30, theme::text, 4));
    g_.text(upper(f.status), rx + 76, top + 68, mono(13, f.online ? theme::ok : theme::faint, 2));
    g_.rect(rx + 40, top + 100, rw - 80, 1, theme::line);
    // Messages, newest at the bottom; drop the oldest until the thread fits.
    TextStyle mt = head(20, theme::text, 0, false);
    mt.wrap = rw * 0.6f;
    mt.lineHeight = 1.05f;
    float availH = bottom - top - 140 - 24;
    std::vector<float> mh;
    float tot = 0;
    size_t start = chat_.size();
    while (start > 0) {
      float h = g_.measure(chat_[start - 1].body, mt).y + 30;
      if (tot + h + 10 > availH) break;
      tot += h + 10;
      --start;
      mh.insert(mh.begin(), h);
    }
    if (chat_.empty()) g_.text("No messages yet. Say hello.", rx + 40, top + 130, mono(16, theme::faint, 1));
    float my = top + 120;
    for (size_t i = start; i < chat_.size(); ++i) {
      float h = mh[i - start];
      Vec2 m = g_.measure(chat_[i].body, mt);
      float bw = m.x + 36;
      float bx = chat_[i].mine ? rx + rw - 40 - bw : rx + 40;
      g_.rrect(bx, my, bw, h, 12, chat_[i].mine ? Color(120, 52, 20, 255) : theme::panelHi);
      TextStyle c = mt;
      c.color = theme::text;
      g_.text(chat_[i].body, bx + 18, my + 15, c);
      my += h + 10;
    }
    hints(st, {{Action::Accept, "MESSAGE"}, {Action::Y, "REFRESH"}});
  }
}

void Pages::drawProfile(double now, PadStyle st) {
  const float W = g_.W();
  if (!proLoaded_) {
    drawEmpty(proLoading_ ? "LOADING PROFILE" : "NOTHING TO SHOW", "", proLoading_, now);
    hints(st, {});
    return;
  }
  float top = kTop + 8, bottom = kBottom, lw = 620, rx = kMargin + lw + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, lw, bottom - top, false);
  panel(g_, rx, top, rw, bottom - top, true);

  // Identity card.
  float cx = kMargin + 100, cy = top + 120;
  g_.glow(cx, cy, 130, 130, theme::magma.alpha(0.25f));
  g_.circle(cx, cy, 62, theme::panelHi);
  g_.ring(cx, cy, 62, 3, theme::magma);
  TextStyle in = head(64, theme::magma, 0);
  in.align = Align::Center;
  std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(gamertag_.empty() ? '?' : gamertag_[0]))));
  g_.text(ini, cx, cy - g_.measure(ini, in).y / 2, in);
  float tx = cx + 92;
  g_.text(g_.ellipsize(upper(gamertag_), head(38, theme::text, 3), lw - 260), tx, top + 72, head(38, theme::text, 3));
  g_.text("LEVEL " + std::to_string(level_), tx, top + 124, mono(16, theme::ember, 4));
  ui::progressBar(g_, tx, top + 156, lw - 260, 8, xp_ / 4000.f, theme::ember);
  g_.text(std::to_string(xp_) + " / 4000 XP", tx, top + 172, mono(12, theme::faint, 2));
  if (!memberSince_.empty()) g_.text("MEMBER SINCE " + fmtDate(memberSince_), kMargin + 36, top + 228, mono(13, theme::faint, 3));

  // Bio.
  TextStyle bs = head(20, theme::dim, 0, false);
  bs.wrap = lw - 72;
  g_.text(bio_.empty() ? "No bio yet." : bio_, kMargin + 36, top + 260, bs);

  // Stat tiles.
  struct T { std::string l, v; };
  char hb[24];
  std::snprintf(hb, sizeof hb, "%.1f", hours_);
  std::vector<T> tiles = {{"TITLES", std::to_string(titles_)}, {"HOURS PLAYED", hb}, {"ACHIEVEMENTS", std::to_string(achCount_)}, {"FRIENDS", std::to_string(friendCount_)}};
  for (int i = 0; i < 4; ++i) {
    float x = kMargin + 36 + (i % 2) * ((lw - 90) / 2 + 18), y = top + 350 + (i / 2) * 96;
    g_.rrect(x, y, (lw - 90) / 2, 80, 8, theme::panel);
    g_.text(tiles[i].l, x + 18, y + 12, mono(12, theme::faint, 3));
    g_.text(tiles[i].v, x + 18, y + 32, head(34, theme::text, 1));
  }
  // Most played.
  float py = top + 560;
  g_.text("MOST PLAYED", kMargin + 36, py, mono(13, theme::faint, 4));
  int maxMin = playtime_.empty() ? 1 : std::max(1, playtime_[0].second);
  for (size_t i = 0; i < playtime_.size() && i < 4 && py + 40 + i * 62 < bottom - 30; ++i) {
    float y = py + 30 + i * 62;
    g_.text(g_.ellipsize(upper(playtime_[i].first), head(18, theme::text, 2), lw - 250), kMargin + 36, y, head(18, theme::text, 2));
    TextStyle r = mono(14, theme::dim, 2);
    r.align = Align::Right;
    g_.text(humanPlaytime(playtime_[i].second), kMargin + lw - 36, y + 3, r);
    ui::progressBar(g_, kMargin + 36, y + 30, lw - 72, 6, static_cast<float>(playtime_[i].second) / maxMin, theme::magma);
  }

  // Right: bio / settings / devices as one navigable list.
  static const char* sl[5] = {"SHOW MY ACTIVITY", "ALLOW FRIEND REQUESTS", "MESSAGES FROM NON-FRIENDS", "CLOUD SAVES", "PATCH NOTE EMAILS"};
  static const char* sd[5] = {"Friends can see what you've been playing", "Other players can send you friend requests",
                              "Anyone can message you, not just friends", "Back up game saves to your account",
                              "Email me when a game I own updates"};
  std::vector<float> hs;
  for (auto& r : prows_) hs.push_back(r.kind == ProfileRow::Header ? 50.f : r.kind == ProfileRow::Bio ? 84.f : 80.f);
  int first = windowStart(hs, pSel_, bottom - top - 24);
  float y = top + 12;
  for (int i = first; i < static_cast<int>(prows_.size()); ++i) {
    if (y + hs[i] > bottom - 8) break;
    const ProfileRow& r = prows_[i];
    bool on = i == pSel_;
    if (r.kind == ProfileRow::Header) {
      g_.text(r.idx == 0 ? "PRIVACY & SETTINGS" : "YOUR DEVICES", rx + 40, y + 22, mono(13, theme::faint, 4));
    } else {
      rowBg(g_, rx + 10, y, rw - 20, hs[i] - 6, on, true);
      if (r.kind == ProfileRow::Bio) {
        g_.text("EDIT BIO", rx + 40, y + 14, head(22, on ? theme::text : theme::dim, 3));
        g_.text(g_.ellipsize(bio_.empty() ? "Tell people about yourself" : bio_, mono(14, theme::faint, 1), rw - 100), rx + 40, y + 46, mono(14, theme::faint, 1));
      } else if (r.kind == ProfileRow::Setting) {
        g_.text(sl[r.idx], rx + 40, y + 12, head(21, on ? theme::text : theme::dim, 3));
        g_.text(sd[r.idx], rx + 40, y + 44, mono(13, theme::faint, 1));
        toggle(g_, rx + rw - 40 - 64, y + 20, settings_[r.idx]);
      } else {
        const Device& d = devices_[r.idx];
        g_.text(g_.ellipsize(upper(d.name), head(21, theme::text, 3), rw - 380), rx + 40, y + 12, head(21, on ? theme::text : theme::dim, 3));
        std::string sub = upper(d.platform) + "  ·  " + std::to_string(d.installs.size()) + (d.installs.size() == 1 ? " INSTALL" : " INSTALLS");
        if (!d.lastSeen.empty()) sub += "  ·  SEEN " + fmtDate(d.lastSeen);
        g_.text(g_.ellipsize(sub, mono(13, theme::faint, 1.5f), rw - 100), rx + 40, y + 46, mono(13, theme::faint, 1.5f));
        if (on) ui::prompt(g_, st, Action::X, "FORGET", rx + rw - 40 - ui::promptWidth(g_, st, Action::X, "FORGET", 20), y + 14, 20);
      }
    }
    y += hs[i];
  }
  const ProfileRow& sel = prows_[pSel_];
  std::vector<std::pair<Action, std::string>> h;
  if (sel.kind == ProfileRow::Bio) h.push_back({Action::Accept, "EDIT"});
  else if (sel.kind == ProfileRow::Setting) h.push_back({Action::Accept, "TOGGLE"});
  else if (sel.kind == ProfileRow::Device) h.push_back({Action::X, "FORGET DEVICE"});
  h.push_back({Action::Y, "REFRESH"});
  hints(st, h);
}

}  // namespace vos
