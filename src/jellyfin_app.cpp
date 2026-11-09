#include "jellyfin_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "json.hpp"
#include "widgets.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vos {

using nlohmann::json;

namespace {

constexpr float kM = 96;  // side margin
const Color kJf{170, 92, 195};   // Jellyfin purple
const Color kJf2{0, 164, 220};   // ... and blue

TextStyle head(float size, Color c, bool bold = true) {
  TextStyle t;
  t.font = Font::Head;
  t.size = size;
  t.color = c;
  t.bold = bold;
  return t;
}
TextStyle body(float size, Color c) {
  TextStyle t;
  t.font = Font::Mono;
  t.size = size;
  t.color = c;
  return t;
}

std::string clipText(std::string s, size_t n) {
  if (s.size() <= n) return s;
  s.resize(n);
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // don't cut a UTF-8 sequence
  return s + "…";
}

std::string fmtTime(double sec) {
  int s = static_cast<int>(sec < 0 ? 0 : sec);
  char b[32];
  if (s >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
  return b;
}

std::string runtimeText(int min) {
  if (min <= 0) return {};
  if (min < 60) return std::to_string(min) + "m";
  return std::to_string(min / 60) + "h " + std::to_string(min % 60) + "m";
}

std::string loadDeviceId() {
  std::string text;
  if (readFile(dataDir() / "jellyfin.json", text)) {
    json j = json::parse(text, nullptr, false);
    if (j.is_object() && j.contains("deviceId") && j["deviceId"].is_string() && !j["deviceId"].get<std::string>().empty())
      return j["deviceId"].get<std::string>();
  }
  std::string id = randomToken(12);
  id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }), id.end());
  if (id.size() < 8) id = "volcano" + std::to_string(static_cast<long>(nowSeconds() * 1000));
  return id;
}

}  // namespace

JellyfinApp::JellyfinApp(AppContext& c) : AppModule(c), jf_(loadDeviceId(), "Volcano OS", VOS_VERSION) {
  demo_ = c.demo;
  loadState();
}

AppModule::Info JellyfinApp::info() const {
  return {"jellyfin", "Jellyfin", "Your media server",
          "Movies, shows and more from your own Jellyfin server. Pick up where you left off on any device.",
          "file://" + (assetDir() / "apps" / "jellyfin.png").string(), kJf};
}

std::string JellyfinApp::status() const {
  JfSession s = jf_.session();
  if (!s.valid()) return "Not connected";
  return s.userName + " on " + (s.serverName.empty() ? s.server : s.serverName);
}

// ---- persistence -------------------------------------------------------------------------------------

void JellyfinApp::loadState() {
  std::string text;
  JfSession s;
  if (readFile(dataDir() / "jellyfin.json", text)) {
    json j = json::parse(text, nullptr, false);
    if (j.is_object()) {
      deviceId_ = j.value("deviceId", "");
      server_ = j.value("lastServer", "");
      serverName_ = j.value("lastServerName", "");
      if (j.contains("session") && j["session"].is_object()) {
        const json& x = j["session"];
        s.server = x.value("server", "");
        s.serverName = x.value("serverName", "");
        s.token = x.value("token", "");
        s.userId = x.value("userId", "");
        s.userName = x.value("userName", "");
      }
    }
  }
  if (s.valid()) jf_.setSession(s);
}

void JellyfinApp::saveState() {
  if (demo_) return;
  JfSession s = jf_.session();
  json j = {{"deviceId", deviceId_.empty() ? loadDeviceId() : deviceId_}, {"lastServer", server_}, {"lastServerName", serverName_}};
  if (s.valid())
    j["session"] = {{"server", s.server}, {"serverName", s.serverName}, {"token", s.token}, {"userId", s.userId}, {"userName", s.userName}};
  const fs::path p = dataDir() / "jellyfin.json";
  writeFileAtomic(p, j.dump());
#ifndef _WIN32
  ::chmod(p.string().c_str(), 0600);  // holds an access token
#endif
}

void JellyfinApp::signOut() {
  JfSession s = jf_.session();
  ++gen_;
  jf_.setSession({});
  rows_.clear();
  stack_.clear();
  grid_.clear();
  homeLoadedAt_ = 0;
  saveState();
  logf("jellyfin: signed out");
}

bool JellyfinApp::failed(int status, const std::string& error) {
  if (status == 401) {
    c_.toast("Your Jellyfin session expired. Sign in again.", theme::warn);
    signOut();
    beginSetup();
    return true;
  }
  c_.toast(error.empty() ? "Jellyfin request failed" : error, theme::err);
  return false;
}

// ---- entry ---------------------------------------------------------------------------------------------

void JellyfinApp::open() {
  openedAt_ = nowSeconds();
  stack_.clear();
  if (demo_) {
    if (mode_ == Mode::Setup) mode_ = Mode::Home;
    return;
  }
  if (!jf_.signedIn()) {
    beginSetup();
    return;
  }
  mode_ = Mode::Home;
  loadHome(false);
}

bool JellyfinApp::handle(Action a) {
  switch (mode_) {
    case Mode::Setup: return handleSetup(a);
    case Mode::Home: return handleHome(a);
    case Mode::Grid: return handleGrid(a);
    case Mode::Detail: return handleDetail(a);
  }
  return false;
}

void JellyfinApp::draw(double now, float dt, PadStyle st) {
  switch (mode_) {
    case Mode::Setup: drawSetup(now, st); break;
    case Mode::Home: drawHome(now, dt, st); break;
    case Mode::Grid: drawGrid(now, dt, st); break;
    case Mode::Detail: drawDetail(now, dt, st); break;
  }
}

void JellyfinApp::tick(double now) {
  if (demo_) return;
  if (mode_ == Mode::Setup && step_ == Step::QuickConnect) {
    if (now - qcStarted_ > 540) {  // Jellyfin expires codes after ~10 minutes
      startQuickConnect();
    } else if (!qcBusy_ && now - qcPolledAt_ > 3) {
      pollQuickConnect();
    }
  }
}

// ---- setup -------------------------------------------------------------------------------------------------

void JellyfinApp::beginSetup() {
  mode_ = Mode::Setup;
  step_ = Step::Servers;
  setupSel_ = 0;
  setupError_.clear();
  found_.clear();
  discoverServers();
}

void JellyfinApp::discoverServers() {
  if (discovering_ || demo_) return;
  discovering_ = true;
  const int gen = gen_;
  c_.pool.run<std::vector<JfServer>>(
      c_.main, [] { return Jellyfin::discover(1800); },
      [this, gen](std::vector<JfServer> v) {
        discovering_ = false;
        if (gen != gen_) return;
        found_ = std::move(v);
        logf("jellyfin: discovery found %d server(s)", static_cast<int>(found_.size()));
      });
}

std::vector<std::string> JellyfinApp::setupChoices() const {
  std::vector<std::string> v;
  switch (step_) {
    case Step::Servers:
      for (auto& s : found_) v.push_back(s.name.empty() ? s.address : s.name);
      if (!server_.empty()) {
        bool listed = false;
        for (auto& s : found_) listed |= s.address == server_;
        if (!listed) v.push_back(serverName_.empty() ? server_ : serverName_);
      }
      v.push_back("Enter an address");
      break;
    case Step::QuickConnect: v = {"Use a username and password", "Choose another server"}; break;
    case Step::Password:
      v = {"Username   " + (user_.empty() ? std::string("—") : user_),
           "Password   " + (pass_.empty() ? std::string("—") : std::string(std::min<size_t>(pass_.size(), 16), '*')), "Sign in",
           "Choose another server"};
      break;
    default: break;
  }
  return v;
}

void JellyfinApp::chooseServer(const std::string& raw) {
  const std::string address = Jellyfin::normalizeAddress(raw);
  if (address.empty()) return;
  step_ = Step::Probing;
  setupError_.clear();
  const int gen = gen_;
  struct Probe { JfResult<JfServer> srv; bool qc = false; };
  c_.pool.run<Probe>(
      c_.main,
      [this, address] {
        Probe p;
        p.srv = jf_.probe(address);
        if (p.srv.ok) p.qc = jf_.quickConnectEnabled(address).value;
        return p;
      },
      [this, gen, address](Probe p) {
        if (gen != gen_) return;
        if (!p.srv.ok) {
          step_ = Step::Servers;
          setupError_ = p.srv.error;
          return;
        }
        server_ = address;
        serverName_ = p.srv.value.name;
        saveState();
        setupSel_ = 0;
        if (p.qc) startQuickConnect();
        else step_ = Step::Password;
      });
}

void JellyfinApp::startQuickConnect() {
  step_ = Step::QuickConnect;
  qc_ = {};
  qcStarted_ = qcPolledAt_ = nowSeconds();
  qcBusy_ = true;
  setupSel_ = 0;
  const int gen = gen_;
  c_.pool.run<JfResult<JfQuickConnect>>(
      c_.main, [this, s = server_] { return jf_.quickConnectStart(s); },
      [this, gen](JfResult<JfQuickConnect> r) {
        if (gen != gen_) return;
        qcBusy_ = false;
        if (!r.ok) {
          setupError_ = r.error;
          step_ = Step::Password;
          return;
        }
        qc_ = r.value;
        qcPolledAt_ = nowSeconds();
      });
}

void JellyfinApp::pollQuickConnect() {
  if (qc_.secret.empty()) return;
  qcBusy_ = true;
  qcPolledAt_ = nowSeconds();
  const int gen = gen_;
  c_.pool.run<JfResult<JfSession>>(
      c_.main,
      [this, s = server_, sec = qc_.secret] {
        auto p = jf_.quickConnectPoll(s, sec);
        if (!p.ok || !p.value) {
          JfResult<JfSession> none;
          none.status = p.ok ? -1 : p.status;  // -1: not approved yet
          none.error = p.error;
          return none;
        }
        return jf_.signInQuickConnect(s, sec);
      },
      [this, gen](JfResult<JfSession> r) {
        if (gen != gen_) return;
        qcBusy_ = false;
        if (r.ok) {
          r.value.serverName = serverName_;
          finishSignIn(r.value);
        } else if (r.status != -1) {
          setupError_ = r.error;
          step_ = Step::Password;
        }
      });
}

void JellyfinApp::signInPassword() {
  if (user_.empty()) {
    setupError_ = "Enter your username first";
    return;
  }
  step_ = Step::Signing;
  setupError_.clear();
  const int gen = gen_;
  c_.pool.run<JfResult<JfSession>>(
      c_.main, [this, s = server_, u = user_, p = pass_] { return jf_.signInPassword(s, u, p); },
      [this, gen](JfResult<JfSession> r) {
        if (gen != gen_) return;
        pass_.clear();
        if (!r.ok) {
          setupError_ = r.error;
          step_ = Step::Password;
          setupSel_ = 1;
          return;
        }
        r.value.serverName = serverName_;
        finishSignIn(r.value);
      });
}

void JellyfinApp::finishSignIn(const JfSession& s) {
  jf_.setSession(s);
  saveState();
  user_.clear();
  pass_.clear();
  c_.toast("Signed in to Jellyfin as " + s.userName, theme::ok);
  mode_ = Mode::Home;
  stack_.clear();
  loadHome(true);
}

bool JellyfinApp::handleSetup(Action a) {
  auto ch = setupChoices();
  const int n = static_cast<int>(ch.size());
  if (step_ == Step::Probing || step_ == Step::Signing) {
    if (a == Action::Back) { ++gen_; step_ = Step::Servers; setupSel_ = 0; }  // abandon the request
    return false;
  }
  if (a == Action::Up && n) setupSel_ = (setupSel_ + n - 1) % n;
  else if (a == Action::Down && n) setupSel_ = (setupSel_ + 1) % n;
  else if (a == Action::Y && step_ == Step::Servers) discoverServers();
  else if (a == Action::Back) {
    if (step_ == Step::Servers) return true;
    step_ = Step::Servers;
    setupSel_ = 0;
    setupError_.clear();
    ++gen_;  // abandon a pending Quick Connect poll
    qcBusy_ = false;
  } else if (a == Action::Accept && n) {
    if (step_ == Step::Servers) {
      const int found = static_cast<int>(found_.size());
      const bool hasSaved = n == found + 2;
      if (setupSel_ < found) chooseServer(found_[setupSel_].address);
      else if (hasSaved && setupSel_ == found) chooseServer(server_);
      else
        c_.text("Server address", server_.empty() ? "http://" : server_, 120, [this](const std::string& v) { chooseServer(v); });
    } else if (step_ == Step::QuickConnect) {
      if (setupSel_ == 0) { step_ = Step::Password; setupSel_ = 0; setupError_.clear(); }
      else { step_ = Step::Servers; setupSel_ = 0; }
    } else if (step_ == Step::Password) {
      if (setupSel_ == 0) c_.text("Jellyfin username", user_, 64, [this](const std::string& v) { user_ = v; setupSel_ = 1; });
      else if (setupSel_ == 1) c_.text("Jellyfin password", "", 128, [this](const std::string& v) { pass_ = v; setupSel_ = 2; });
      else if (setupSel_ == 2) signInPassword();
      else { step_ = Step::Servers; setupSel_ = 0; }
    }
  }
  return false;
}

// ---- home --------------------------------------------------------------------------------------------------------

void JellyfinApp::loadHome(bool force) {
  if (demo_ || homeLoading_) return;
  const double now = nowSeconds();
  if (!force && !rows_.empty() && now - homeLoadedAt_ < 45) return;
  homeLoading_ = true;
  const int gen = gen_;
  struct Result { std::vector<Row> rows; int status = 200; std::string error; };
  c_.pool.run<Result>(
      c_.main,
      [this] {
        Result out;
        auto add = [&](const char* title, std::vector<JfItem> items, bool landscape) {
          if (items.empty()) return;
          Row r;
          r.title = title;
          r.items = std::move(items);
          r.landscape = landscape;
          out.rows.push_back(std::move(r));
        };
        auto res = jf_.resume(16);
        if (!res.ok) { out.status = res.status; out.error = res.error; return out; }
        add("Continue watching", res.value, true);
        auto nu = jf_.nextUp(16);
        if (nu.ok) add("Next up", nu.value, true);
        auto vw = jf_.views();
        if (vw.ok) {
          add("Libraries", vw.value, true);
          int shown = 0;
          for (auto& v : vw.value) {
            if (shown >= 4) break;
            if (v.collection != "movies" && v.collection != "tvshows" && v.collection != "homevideos" && v.collection != "boxsets") continue;
            auto lt = jf_.latest(v.id, 16);
            if (lt.ok && !lt.value.empty()) {
              Row r;
              r.title = "Latest in " + v.name;
              r.items = std::move(lt.value);
              out.rows.push_back(std::move(r));
              ++shown;
            }
          }
        }
        return out;
      },
      [this, gen](Result r) {
        homeLoading_ = false;
        if (gen != gen_) return;
        if (r.status != 200) {
          failed(r.status, r.error);
          return;
        }
        std::string keep = focusedItem() ? focusedItem()->id : "";
        rows_ = std::move(r.rows);
        homeLoadedAt_ = nowSeconds();
        row_ = std::min(row_, std::max(0, static_cast<int>(rows_.size()) - 1));
        (void)keep;
        for (auto& row : rows_) row.loaded = true;
      });
}

const JfItem* JellyfinApp::focusedItem() const {
  if (mode_ != Mode::Home || row_ < 0 || row_ >= static_cast<int>(rows_.size())) return nullptr;
  const Row& r = rows_[row_];
  if (r.items.empty()) return nullptr;
  return &r.items[std::min(r.col, static_cast<int>(r.items.size()) - 1)];
}

bool JellyfinApp::handleHome(Action a) {
  const int nrows = static_cast<int>(rows_.size());
  if (row_ < 0) {  // header: Search | Refresh | Sign out
    switch (a) {
      case Action::Left: headSel_ = std::max(0, headSel_ - 1); break;
      case Action::Right: headSel_ = std::min(2, headSel_ + 1); break;
      case Action::Down: if (nrows) row_ = 0; break;
      case Action::Back: return true;
      case Action::Accept:
        if (headSel_ == 0) search();
        else if (headSel_ == 1) loadHome(true);
        else c_.confirm("Sign out of Jellyfin?", "You will need to sign in again to watch.", "SIGN OUT", [this] {
               signOut();
               beginSetup();
             });
        break;
      default: break;
    }
    return false;
  }
  switch (a) {
    case Action::Left: if (nrows) rows_[row_].col = std::max(0, rows_[row_].col - 1); break;
    case Action::Right:
      if (nrows) rows_[row_].col = std::min(static_cast<int>(rows_[row_].items.size()) - 1, rows_[row_].col + 1);
      break;
    case Action::Up: row_ = nrows && row_ > 0 ? row_ - 1 : -1; break;
    case Action::Down: if (row_ + 1 < nrows) ++row_; break;
    case Action::Accept: if (const JfItem* it = focusedItem()) openItem(*it); break;
    case Action::Y:
      if (const JfItem* it = focusedItem())
        if (it->playable()) play(*it, true);
      break;
    case Action::X: search(); break;
    case Action::Start: loadHome(true); break;
    case Action::Back: return true;
    default: break;
  }
  return false;
}

void JellyfinApp::search() {
  c_.text("Search Jellyfin", "", 60, [this](const std::string& term) {
    if (term.empty()) return;
    openGrid("Results for “" + term + "”", "", term);
  });
}

// ---- grid / detail ----------------------------------------------------------------------------------------------------

void JellyfinApp::pushFrame() {
  Frame f;
  f.mode = mode_;
  f.item = det_;
  f.gridTitle = gridTitle_;
  f.gridParent = gridParent_;
  f.gridSearch = gridSearch_;
  f.grid = grid_;
  f.gridTotal = gridTotal_;
  f.gSel = gSel_;
  f.zone = zone_;
  f.season = season_;
  f.ep = ep_;
  f.seasons = seasons_;
  f.episodes = episodes_;
  stack_.push_back(std::move(f));
}

void JellyfinApp::popFrame() {
  if (stack_.empty()) {
    mode_ = Mode::Home;
    return;
  }
  Frame f = std::move(stack_.back());
  stack_.pop_back();
  ++gen_;  // anything still loading for the screen we're leaving is stale
  gridLoading_ = detLoading_ = false;
  mode_ = f.mode;
  det_ = f.item;
  gridTitle_ = f.gridTitle;
  gridParent_ = f.gridParent;
  gridSearch_ = f.gridSearch;
  grid_ = std::move(f.grid);
  gridTotal_ = f.gridTotal;
  gSel_ = f.gSel;
  zone_ = f.zone;
  season_ = f.season;
  ep_ = f.ep;
  seasons_ = std::move(f.seasons);
  episodes_ = std::move(f.episodes);
  if (mode_ == Mode::Detail && det_.type == "Series" && seasons_.empty()) loadSeasons();
}

void JellyfinApp::openItem(const JfItem& it) {
  if (it.type == "CollectionFolder" || it.type == "UserView" || it.type == "Folder" || it.type == "BoxSet" || it.type == "Season")
    openGrid(it.name, it.id);
  else
    openDetail(it);
}

void JellyfinApp::openGrid(const std::string& title, const std::string& parentId, const std::string& searchTerm) {
  pushFrame();
  mode_ = Mode::Grid;
  gridTitle_ = title;
  gridParent_ = parentId;
  gridSearch_ = searchTerm;
  grid_.clear();
  gridTotal_ = 0;
  gSel_ = 0;
  gridScroll_ = 0;
  loadGridPage();
}

void JellyfinApp::loadGridPage() {
  if (gridLoading_ || demo_) return;
  gridLoading_ = true;
  const int gen = gen_;
  const int start = static_cast<int>(grid_.size());
  c_.pool.run<JfResult<JfPage>>(
      c_.main,
      [this, p = gridParent_, s = gridSearch_, start] {
        if (p.empty()) return jf_.search(s, 80);
        return jf_.children(p, start, 60);
      },
      [this, gen](JfResult<JfPage> r) {
        gridLoading_ = false;
        if (gen != gen_) return;
        if (!r.ok) {
          failed(r.status, r.error);
          return;
        }
        for (auto& it : r.value.items) grid_.push_back(std::move(it));
        gridTotal_ = std::max(r.value.total, static_cast<int>(grid_.size()));
      });
}

void JellyfinApp::openDetail(const JfItem& it) {
  pushFrame();
  mode_ = Mode::Detail;
  det_ = it;
  zone_ = 0;
  btn_ = 0;
  season_ = ep_ = 0;
  epScroll_ = 0;
  seasons_.clear();
  episodes_.clear();
  if (demo_) {
    if (it.type == "Series") { seasons_ = demoSeasons_; episodes_ = demoEpisodes_; }
    return;
  }
  detLoading_ = true;
  const int gen = gen_;
  c_.pool.run<JfResult<JfItem>>(
      c_.main, [this, id = it.id] { return jf_.item(id); },
      [this, gen](JfResult<JfItem> r) {
        detLoading_ = false;
        if (gen != gen_) return;
        if (r.ok) det_ = r.value;
        else failed(r.status, r.error);
      });
  if (it.type == "Series") loadSeasons();
}

void JellyfinApp::loadSeasons() {
  if (demo_) return;
  const int gen = gen_;
  c_.pool.run<JfResult<std::vector<JfItem>>>(
      c_.main, [this, id = det_.id] { return jf_.seasons(id); },
      [this, gen](JfResult<std::vector<JfItem>> r) {
        if (gen != gen_ || mode_ != Mode::Detail) return;
        if (!r.ok) { failed(r.status, r.error); return; }
        seasons_ = std::move(r.value);
        season_ = 0;
        for (size_t i = 0; i < seasons_.size(); ++i)
          if (seasons_[i].unplayedCount > 0) { season_ = static_cast<int>(i); break; }
        loadEpisodes();
      });
}

void JellyfinApp::loadEpisodes() {
  if (demo_ || season_ < 0 || season_ >= static_cast<int>(seasons_.size())) return;
  const int gen = gen_;
  const int want = season_;
  episodes_.clear();
  c_.pool.run<JfResult<std::vector<JfItem>>>(
      c_.main, [this, sid = det_.id, sea = seasons_[season_].id] { return jf_.episodes(sid, sea); },
      [this, gen, want](JfResult<std::vector<JfItem>> r) {
        if (gen != gen_ || mode_ != Mode::Detail || want != season_) return;
        if (!r.ok) { failed(r.status, r.error); return; }
        episodes_ = std::move(r.value);
        ep_ = 0;
        for (size_t i = 0; i < episodes_.size(); ++i)
          if (!episodes_[i].played) { ep_ = static_cast<int>(i); break; }
        epScroll_ = 0;
      });
}

bool JellyfinApp::handleGrid(Action a) {
  const int cols = 7;
  const int n = static_cast<int>(grid_.size());
  switch (a) {
    case Action::Left: if (gSel_ % cols) --gSel_; break;
    case Action::Right: if (gSel_ % cols != cols - 1 && gSel_ + 1 < n) ++gSel_; break;
    case Action::Up: if (gSel_ >= cols) gSel_ -= cols; break;
    case Action::Down: if (gSel_ + cols < n) gSel_ += cols; else if (gSel_ < n - 1) gSel_ = n - 1; break;
    case Action::LB: gSel_ = std::max(0, gSel_ - cols * 3); break;
    case Action::RB: gSel_ = std::min(std::max(0, n - 1), gSel_ + cols * 3); break;
    case Action::Accept: if (gSel_ < n) openItem(grid_[gSel_]); break;
    case Action::Y:
      if (gSel_ < n && grid_[gSel_].playable()) play(grid_[gSel_], true);
      break;
    case Action::Back: popFrame(); break;
    default: break;
  }
  if (gridSearch_.empty() && n < gridTotal_ && gSel_ >= n - cols * 2) loadGridPage();
  return false;
}

bool JellyfinApp::handleDetail(Action a) {
  const bool series = det_.type == "Series";
  const int buttons = 2;
  auto clampZone = [&] {
    if (zone_ == 1 && seasons_.empty()) zone_ = 0;
    if (zone_ == 2 && episodes_.empty()) zone_ = seasons_.empty() ? 0 : 1;
  };
  switch (a) {
    case Action::Left:
      if (zone_ == 0) btn_ = std::max(0, btn_ - 1);
      else if (zone_ == 1) {
        if (season_ > 0) { --season_; loadEpisodes(); }
      } else ep_ = std::max(0, ep_ - 1);
      break;
    case Action::Right:
      if (zone_ == 0) btn_ = std::min(buttons - 1, btn_ + 1);
      else if (zone_ == 1) {
        if (season_ + 1 < static_cast<int>(seasons_.size())) { ++season_; loadEpisodes(); }
      } else ep_ = std::min(static_cast<int>(episodes_.size()) - 1, ep_ + 1);
      break;
    case Action::Down:
      if (series && zone_ < 2) { ++zone_; clampZone(); }
      break;
    case Action::Up:
      if (zone_ > 0) { --zone_; clampZone(); }
      break;
    case Action::Accept:
      if (zone_ == 0) {
        if (btn_ == 0) {
          if (series) {
            // Play the next unwatched episode (or the first one we know of).
            const JfItem* e = nullptr;
            for (auto& x : episodes_) if (!x.played) { e = &x; break; }
            if (!e && !episodes_.empty()) e = &episodes_.front();
            if (e) play(*e, true);
            else c_.toast("Loading episodes…", theme::dim);
          } else {
            play(det_, true);
          }
        } else {
          const bool want = !det_.played;
          det_.played = want;
          if (want) det_.positionTicks = 0;
          if (!demo_) c_.pool.submit([this, id = det_.id, want] { jf_.setPlayed(id, want); });
          c_.toast(want ? "Marked as watched" : "Marked as unwatched", theme::dim);
          homeLoadedAt_ = 0;
        }
      } else if (zone_ == 1) {
        zone_ = episodes_.empty() ? 1 : 2;
      } else if (ep_ < static_cast<int>(episodes_.size())) {
        openDetail(episodes_[ep_]);
      }
      break;
    case Action::Y:
      if (zone_ == 2 && ep_ < static_cast<int>(episodes_.size())) play(episodes_[ep_], true);
      else if (det_.playable()) play(det_, true);
      break;
    case Action::X:  // play from the start
      if (det_.playable()) play(det_, false);
      break;
    case Action::Back: popFrame(); break;
    default: break;
  }
  return false;
}

// ---- playback -------------------------------------------------------------------------------------------------------------

std::string JellyfinApp::findMpv() {
  if (const char* e = std::getenv("VOS_MPV"); e && *e && fs::exists(e)) return e;
  std::vector<fs::path> dirs;
  if (const char* p = std::getenv("PATH")) {
    std::string s = p;
    size_t i = 0;
    while (i <= s.size()) {
      size_t j = s.find(':', i);
      dirs.emplace_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
      if (j == std::string::npos) break;
      i = j + 1;
    }
  }
  for (const char* d : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/snap/bin", "/Applications/mpv.app/Contents/MacOS"}) dirs.emplace_back(d);
  for (auto& d : dirs) {
    std::error_code ec;
    fs::path c = d / "mpv";
    if (!d.empty() && fs::is_regular_file(c, ec)) return c.string();
  }
  return {};
}

void JellyfinApp::play(const JfItem& it, bool resume) {
  std::string title = it.type == "Episode" && !it.seriesName.empty()
                          ? it.seriesName + " · S" + std::to_string(it.seasonNo) + "E" + std::to_string(it.episodeNo) + " · " + it.name
                          : it.name;
  if (demo_) {
    c_.toast("Demo: would play " + title, theme::ok);
    return;
  }
  const std::string mpv = findMpv();
  if (mpv.empty()) {
    c_.toast("mpv isn't installed on this device, so Jellyfin can't play video", theme::err);
    return;
  }
  JfSession s = jf_.session();
  if (!s.valid()) return;

  std::error_code ec;
  sockPath_ = (fs::temp_directory_path(ec) / ("vos-mpv-" + std::to_string(static_cast<long>(::getpid())) + ".sock")).string();
  fs::remove(sockPath_, ec);

  AppLaunch l;
  l.title = title;
  l.exe = mpv;
  l.workdir = dataDir();
  l.args = {"--fullscreen", "--no-terminal", "--force-window=yes", "--idle=no", "--keep-open=no", "--osc=no",
            "--osd-level=1", "--osd-duration=2500", "--hwdec=auto-safe",
            "--input-ipc-server=" + sockPath_, "--force-media-title=" + title,
            "--http-header-fields=X-Emby-Token: " + s.token};
  if (resume && it.positionTicks > 0) l.args.push_back("--start=" + std::to_string(it.positionTicks / 10000000LL));
  l.args.push_back(jf_.streamUrl(it));

  ipc_.reset();
  if (!c_.launch(l)) return;
  playing_ = it;
  playSession_ = randomToken(9);
  playSession_.erase(std::remove_if(playSession_.begin(), playSession_.end(), [](char ch) { return !std::isalnum(static_cast<unsigned char>(ch)); }), playSession_.end());
  playStarted_ = nowSeconds();
  lastReport_ = playStarted_;
  lastConnectTry_ = 0;
  reportedStart_ = false;
  lastTicks_ = resume ? it.positionTicks : 0;
  wasPlaying_ = true;
  logf("jellyfin: playing %s (%s)", title.c_str(), it.id.c_str());
}

void JellyfinApp::sessionTick(double now) {
  if (!wasPlaying_) return;
  if (!ipc_.connected() && now - lastConnectTry_ > 0.3 && now - playStarted_ < 30) {
    lastConnectTry_ = now;
    ipc_.connect(sockPath_);
  }
  ipc_.poll();
  if (ipc_.seen) lastTicks_ = static_cast<int64_t>(std::max(0.0, ipc_.position) * 10000000.0);
  if (ipc_.seen && !reportedStart_) {
    reportedStart_ = true;
    c_.pool.submit([this, it = playing_, t = lastTicks_, ps = playSession_] { jf_.reportStart(it, t, ps); });
  }
  if (reportedStart_ && now - lastReport_ >= 10) {
    lastReport_ = now;
    c_.pool.submit([this, it = playing_, t = lastTicks_, p = ipc_.paused, ps = playSession_] { jf_.reportProgress(it, t, p, ps); });
  }
}

bool JellyfinApp::sessionAction(Action a) {
  if (!ipc_.connected()) return true;  // mpv is still starting: swallow the press
  switch (a) {
    case Action::Accept: ipc_.cmd(R"(["cycle","pause"])"); ipc_.cmd(R"(["show-progress"])"); break;
    case Action::Left: ipc_.cmd(R"(["seek",-10,"relative"])"); break;
    case Action::Right: ipc_.cmd(R"(["seek",10,"relative"])"); break;
    case Action::LB: ipc_.cmd(R"(["seek",-60,"relative"])"); break;
    case Action::RB: ipc_.cmd(R"(["seek",60,"relative"])"); break;
    case Action::Up: ipc_.cmd(R"(["add","volume",5])"); break;
    case Action::Down: ipc_.cmd(R"(["add","volume",-5])"); break;
    case Action::X: ipc_.cmd(R"(["osd-auto","cycle","sub"])"); break;
    case Action::Y: ipc_.cmd(R"(["osd-auto","cycle","audio"])"); break;
    case Action::Start:
    case Action::Select: ipc_.cmd(R"(["show-progress"])"); break;
    case Action::Back: ipc_.cmd(R"(["quit"])"); break;
    default: return false;
  }
  return true;
}

void JellyfinApp::sessionEnded(int code) {
  if (!wasPlaying_) return;
  wasPlaying_ = false;
  ipc_.close();
  std::error_code ec;
  fs::remove(sockPath_, ec);
  if (reportedStart_) {
    c_.pool.submit([this, it = playing_, t = lastTicks_, ps = playSession_] { jf_.reportStopped(it, t, ps); });
  } else if (code != 0 || nowSeconds() - playStarted_ < 5) {
    c_.toast("Playback didn't start. See mpv.log in the logs folder.", theme::err);
  }
  // Show the new resume point / watched state.
  homeLoadedAt_ = 0;
  if (mode_ == Mode::Home) loadHome(true);
  else if (mode_ == Mode::Detail && !demo_) {
    const int gen = gen_;
    c_.pool.run<JfResult<JfItem>>(
        c_.main, [this, id = det_.id] { return jf_.item(id); },
        [this, gen](JfResult<JfItem> r) {
          if (gen != gen_ || mode_ != Mode::Detail || !r.ok) return;
          det_ = r.value;
          if (det_.type == "Series") loadEpisodes();
        });
    if (det_.type == "Episode" || det_.type == "Movie") { /* item refresh above */ }
  }
}

std::string JellyfinApp::sessionStatus() const {
  std::string s = playing_.type == "Episode" && !playing_.seriesName.empty() ? playing_.seriesName : playing_.name;
  if (ipc_.seen) s += "  ·  " + fmtTime(ipc_.position) + (ipc_.duration > 0 ? " / " + fmtTime(ipc_.duration) : "");
  return s;
}

// ---- drawing --------------------------------------------------------------------------------------------------------------

std::string JellyfinApp::subLine(const JfItem& it) {
  if (it.type == "Episode") {
    std::string s = "S" + std::to_string(it.seasonNo) + ":E" + std::to_string(it.episodeNo);
    return it.seriesName.empty() ? s : it.seriesName + " · " + s;
  }
  if (it.type == "Series") return it.year ? std::to_string(it.year) : "Series";
  if (it.type == "CollectionFolder" || it.type == "UserView") return "Library";
  return it.year ? std::to_string(it.year) : it.type;
}

std::string JellyfinApp::metaLine(const JfItem& it) {
  std::vector<std::string> p;
  if (it.type == "Episode") p.push_back("S" + std::to_string(it.seasonNo) + ":E" + std::to_string(it.episodeNo));
  else if (it.type == "Series") p.push_back("Series");
  else if (it.type == "Movie") p.push_back("Movie");
  if (it.year) p.push_back(std::to_string(it.year));
  if (!it.officialRating.empty()) p.push_back(it.officialRating);
  if (std::string r = runtimeText(it.runtimeMin); !r.empty()) p.push_back(r);
  if (it.communityRating > 0) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f / 10", it.communityRating);
    p.push_back(b);
  }
  std::string o;
  for (size_t i = 0; i < p.size(); ++i) o += (i ? "  ·  " : "") + p[i];
  return o;
}

std::string JellyfinApp::resumeLabel(const JfItem& it) {
  if (it.positionTicks > 0 && it.runtimeTicks > it.positionTicks) {
    const int left = static_cast<int>((it.runtimeTicks - it.positionTicks) / 600000000LL);
    return "Resume" + (left > 0 ? "  ·  " + runtimeText(left) + " left" : std::string());
  }
  return "Play";
}

std::string JellyfinApp::imgFor(const JfItem& it, bool landscape, int h) const {
  if (demo_) return {};
  if (landscape) {
    if (it.type == "Episode") return jf_.imageUrl(it.id, "Primary", it.primaryTag, h);
    if (!it.backdropTag.empty()) return jf_.imageUrl(it.id, "Backdrop", it.backdropTag, h);
    if (!it.thumbTag.empty()) return jf_.imageUrl(it.id, "Thumb", it.thumbTag, h);
    if (!it.parentBackdropTag.empty()) return jf_.imageUrl(it.parentBackdropId, "Backdrop", it.parentBackdropTag, h);
    return jf_.imageUrl(it.id, "Primary", it.primaryTag, h);
  }
  if (it.type == "Episode" && !it.seriesId.empty()) return jf_.imageUrl(it.seriesId, "Primary", it.seriesPrimaryTag, h);
  return jf_.imageUrl(it.id, "Primary", it.primaryTag, h);
}

std::string JellyfinApp::backdropFor(const JfItem& it) const {
  if (demo_) return {};
  if (!it.backdropTag.empty()) return jf_.imageUrl(it.id, "Backdrop", it.backdropTag, 1080);
  if (!it.parentBackdropTag.empty()) return jf_.imageUrl(it.parentBackdropId, "Backdrop", it.parentBackdropTag, 1080);
  if (it.type == "Episode") return jf_.imageUrl(it.id, "Primary", it.primaryTag, 1080);
  return {};
}

void JellyfinApp::drawBackdrop(const JfItem* it, double now) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  if (it) {
    if (const ArtImage* a = c_.art.get(backdropFor(*it)))
      g.cover(a->tex, a->w, a->h, W - 1340, 0, 1340, 760, c_.art.fade(a, now), 0.3f);
  }
  g.hgrad(0, 0, W * 0.30f, g.H(), Color(14, 11, 10, 255), Color(14, 11, 10, 240));
  g.hgrad(W * 0.30f, 0, W * 0.28f, g.H(), Color(14, 11, 10, 240), Color(14, 11, 10, 110));
  g.hgrad(W * 0.58f, 0, W * 0.22f, g.H(), Color(14, 11, 10, 110), Color(14, 11, 10, 0));
  g.vgrad(0, 440, W, 330, Color(14, 11, 10, 0), Color(14, 11, 10, 255));  // fully dark by the time the backdrop ends (y=760)
}

void JellyfinApp::drawTop(double now, const std::string& crumb) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  const float cy = 76;
  // wordmark: gradient disc + name
  g.circle(kM + 20, cy, 20, kJf);
  g.circle(kM + 28, cy + 4, 14, kJf2.alpha(0.85f));
  g.text("Jellyfin", kM + 56, cy - g.measure("Jellyfin", head(26, theme::text)).y / 2, head(26, theme::text));
  float x = kM + 56 + g.measure("Jellyfin", head(26, theme::text)).x + 18;
  if (!crumb.empty()) {
    TextStyle t = body(20, theme::dim);
    g.text("›  " + g.ellipsize(crumb, t, W * 0.4f), x, cy - g.measure(crumb, t).y / 2, t);
  }
  if (mode_ == Mode::Home) {
    // header actions: search / refresh / sign out
    const char* labels[3] = {"Search", "Refresh", "Sign out"};
    float rx = W - kM;
    for (int i = 2; i >= 0; --i) {
      TextStyle t = head(19, theme::text, false);
      const float w = g.measure(labels[i], t).x + 44;
      rx -= w;
      const bool foc = row_ == -1 && headSel_ == i;
      ui::glass(g, rx, cy - 22, w, 44, 22, 1.f, foc ? 0.95f : 0.7f);
      if (foc) ui::focusRing(g, rx, cy - 22, w, 44, 22);
      g.text(labels[i], rx + 22, cy - g.measure(labels[i], t).y / 2, t);
      rx -= 10;
    }
    TextStyle u = body(18, theme::dim);
    const std::string who = status();
    g.text(who, rx - 14 - g.measure(who, u).x, cy - g.measure(who, u).y / 2, u);
  }
}

void JellyfinApp::drawTile(const JfItem& it, float x, float y, float w, float h, bool landscape, bool focused, double now) {
  Gfx& g = c_.gfx;
  const float r = 14;
  const ArtImage* a = c_.art.get(imgFor(it, landscape, landscape ? 360 : 540));
  if (a && a->tex) {
    g.imageRound(a->tex, a->w, a->h, x, y, w, h, r, c_.art.fade(a, now));
  } else {
    const Color t = ui::tint(it.id.empty() ? it.name : it.id);
    g.rrectGrad(x, y, w, h, r, Color::mix(theme::panelHi, t, 0.25f), theme::panel);
    TextStyle n = head(landscape ? 22 : 20, theme::dim);
    n.align = Align::Center;
    n.wrap = w - 28;
    const std::string label = clipText(it.type == "Episode" && !it.seriesName.empty() ? it.seriesName : it.name, 40);
    Vec2 m = g.measure(label, n);
    g.text(label, x + w / 2, y + h / 2 - m.y / 2, n);
  }
  g.rrectLine(x, y, w, h, r, 1.5f, theme::line);
  // watched / progress
  if (it.progress() > 0.01 && !it.played) {
    g.rrect(x + 12, y + h - 14, w - 24, 5, 2.5f, Color(0, 0, 0, 150));
    g.rrect(x + 12, y + h - 14, (w - 24) * static_cast<float>(std::min(1.0, it.progress())), 5, 2.5f, kJf);
  }
  if (it.played && it.playable()) {
    g.circle(x + w - 24, y + 24, 14, Color(0, 0, 0, 160));
    g.line(x + w - 31, y + 24, x + w - 26, y + 30, 3, theme::ok);
    g.line(x + w - 26, y + 30, x + w - 17, y + 18, 3, theme::ok);
  } else if (it.unplayedCount > 0 && it.folder) {
    TextStyle b = head(15, Color(255, 255, 255));
    const std::string n = std::to_string(it.unplayedCount);
    const float bw = std::max(28.f, g.measure(n, b).x + 18);
    g.rrect(x + w - bw - 10, y + 10, bw, 28, 14, kJf);
    g.text(n, x + w - bw - 10 + (bw - g.measure(n, b).x) / 2, y + 10 + (28 - g.measure(n, b).y) / 2, b);
  }
  if (focused) ui::focusRing(g, x, y, w, h, r);
}

void JellyfinApp::drawHome(double now, float dt, PadStyle) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  const JfItem* fi = focusedItem();
  drawBackdrop(fi, now);
  drawTop(now, "");

  if (fi) {
    // headline block
    const bool ep = fi->type == "Episode";
    TextStyle kicker = head(17, kJf2, true);
    g.text(ep ? "Episode" : fi->type == "Series" ? "Series" : fi->type == "Movie" ? "Movie" : fi->type == "CollectionFolder" ? "Library" : fi->type, kM, 168, kicker);
    TextStyle t = head(58, theme::text, true);
    const std::string title = ep && !fi->seriesName.empty() ? fi->seriesName : fi->name;
    g.text(g.ellipsize(title, t, 1000), kM, 196, t);
    TextStyle m = body(21, theme::dim);
    std::string meta = ep ? fi->name + "   " + metaLine(*fi) : metaLine(*fi);
    g.text(g.ellipsize(meta, m, 1100), kM, 276, m);
    if (!fi->overview.empty()) {
      TextStyle o = body(20, Color(214, 203, 195));
      o.wrap = 820;
      g.text(clipText(fi->overview, 230), kM, 322, o);
    }
  }

  // rows
  if (rows_.empty()) {
    if (homeLoading_ || demo_ == false) {
      ui::spinner(g, W / 2, 700, 26, now);
      TextStyle s = body(22, theme::dim);
      s.align = Align::Center;
      g.text(homeLoading_ ? "Loading your library" : "Nothing to show yet. Open a library from Search.", W / 2, 750, s);
    }
    return;
  }
  const float top = 560;
  float yAcc = 0, target = 0;
  std::vector<float> ys;
  for (size_t i = 0; i < rows_.size(); ++i) {
    ys.push_back(yAcc);
    if (static_cast<int>(i) == row_) target = yAcc;
    yAcc += rows_[i].landscape ? 330.f : 400.f;
  }
  rowScroll_ += (target - rowScroll_) * std::min(1.f, dt * 9.f);
  g.clip(0, top - 44, W, g.H() - (top - 44));
  for (size_t i = 0; i < rows_.size(); ++i) {
    Row& r = rows_[i];
    const float ry = top + ys[i] - rowScroll_;
    if (ry > g.H() || ry < top - 420) continue;
    const bool onRow = static_cast<int>(i) == row_;
    g.text(r.title, kM, ry - 40, head(24, onRow ? theme::text : theme::dim));
    const float tw = r.landscape ? 320 : 180, th = r.landscape ? 180 : 270, gap = 20;
    const float tx = std::max(0.f, (r.col - 1) * (tw + gap));
    r.scroll += (tx - r.scroll) * std::min(1.f, dt * 10.f);
    for (size_t k = 0; k < r.items.size(); ++k) {
      const float x = kM - r.scroll + k * (tw + gap);
      if (x > W || x + tw < 0) continue;
      const bool foc = onRow && static_cast<int>(k) == r.col;
      drawTile(r.items[k], x, ry, tw, th, r.landscape, foc, now);
      TextStyle lt = head(18, foc ? theme::text : theme::dim, foc);
      const std::string nm = r.items[k].type == "Episode" && !r.items[k].seriesName.empty() ? r.items[k].seriesName : r.items[k].name;
      g.text(g.ellipsize(nm, lt, tw), x, ry + th + 14, lt);
      TextStyle st = body(15, theme::faint);
      g.text(g.ellipsize(subLine(r.items[k]), st, tw), x, ry + th + 40, st);
    }
  }
  g.unclip();
}

void JellyfinApp::drawGrid(double now, float dt, PadStyle) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  drawTop(now, gridTitle_);
  const int cols = 7;
  const float gap = 22, tw = (W - 2 * kM - gap * (cols - 1)) / cols, th = tw * 1.5f, stride = th + 70;
  const int n = static_cast<int>(grid_.size());
  const int selRow = gSel_ / cols;
  const float target = std::max(0.f, (selRow - 1) * stride);  // keep the selected row second from the top
  gridScroll_ += (target - gridScroll_) * std::min(1.f, dt * 10.f);
  g.text(std::to_string(gridTotal_ ? gridTotal_ : n) + " items", kM, 124, body(18, theme::faint));
  g.clip(0, 160, W, g.H() - 160);
  for (int i = 0; i < n; ++i) {
    const float x = kM + (i % cols) * (tw + gap);
    const float y = 190 + (i / cols) * stride - gridScroll_;
    if (y > g.H() || y + stride < 160) continue;
    const bool foc = i == gSel_;
    drawTile(grid_[i], x, y, tw, th, false, foc, now);
    TextStyle lt = head(17, foc ? theme::text : theme::dim, foc);
    g.text(g.ellipsize(grid_[i].name, lt, tw), x, y + th + 12, lt);
    TextStyle st = body(14, theme::faint);
    g.text(g.ellipsize(subLine(grid_[i]), st, tw), x, y + th + 38, st);
  }
  g.unclip();
  if (n == 0) {
    TextStyle s = body(22, theme::dim);
    s.align = Align::Center;
    if (gridLoading_) ui::spinner(g, W / 2, 520, 26, now);
    g.text(gridLoading_ ? "Loading" : "Nothing here", W / 2, 570, s);
  } else if (gridLoading_) {
    ui::spinner(g, W - kM - 16, 124, 12, now);
  }
}

void JellyfinApp::drawDetail(double now, float dt, PadStyle) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  const bool series = det_.type == "Series";
  drawBackdrop(&det_, now);
  drawTop(now, series || det_.type == "Movie" ? det_.name : det_.seriesName);

  // poster
  const float pw = 270, ph = 405, py = 150;
  const ArtImage* pa = c_.art.get(imgFor(det_, false, 600));
  if (pa && pa->tex) g.imageRound(pa->tex, pa->w, pa->h, kM, py, pw, ph, 16, c_.art.fade(pa, now));
  else {
    g.rrectGrad(kM, py, pw, ph, 16, Color::mix(theme::panelHi, ui::tint(det_.id.empty() ? det_.name : det_.id), 0.25f), theme::panel);
  }
  g.rrectLine(kM, py, pw, ph, 16, 1.5f, theme::line);

  const float x = kM + pw + 56;
  float y = 150;
  const bool ep = det_.type == "Episode";
  TextStyle t = head(54, theme::text, true);
  const std::string title = ep && !det_.seriesName.empty() ? det_.seriesName : det_.name;
  g.text(g.ellipsize(title, t, W - x - kM), x, y, t);
  y += 74;
  if (ep) {
    g.text(g.ellipsize(det_.name, head(30, theme::ember, false), W - x - kM), x, y, head(30, theme::ember, false));
    y += 46;
  }
  TextStyle m = body(21, theme::dim);
  g.text(metaLine(det_), x, y, m);
  y += 38;
  float cx = x;
  for (auto& gn : det_.genres) {
    cx += ui::chip(g, gn, cx, y, theme::dim, theme::line, 15) + 8;
  }
  if (!det_.genres.empty()) y += 44;
  TextStyle o = body(21, Color(214, 203, 195));
  o.wrap = std::min(980.f, W - x - kM);
  const std::string ov = clipText(det_.overview.empty() && detLoading_ ? "" : det_.overview, 420);
  g.text(ov, x, y, o);
  y += ov.empty() ? 0 : g.measure(ov, o).y;

  // buttons
  const float by = std::max(y + 30, 470.f);
  const std::string playLabel = series ? "Play next" : resumeLabel(det_);
  const std::string labels[2] = {playLabel, det_.played ? "Mark unwatched" : "Mark watched"};
  float bx = x;
  for (int i = 0; i < 2; ++i) {
    TextStyle bt = head(21, i == 0 ? Color(22, 15, 11) : theme::text, true);
    const float bw = g.measure(labels[i], bt).x + 64;
    const bool foc = zone_ == 0 && btn_ == i;
    if (i == 0) g.rrectGrad(bx, by, bw, 58, 29, theme::ember, theme::magma);
    else ui::glass(g, bx, by, bw, 58, 29, 1.f, 0.8f);
    if (foc) ui::focusRing(g, bx, by, bw, 58, 29);
    g.text(labels[i], bx + 32, by + (58 - g.measure(labels[i], bt).y) / 2, bt);
    bx += bw + 14;
  }
  if (det_.progress() > 0.01 && !det_.played) {
    g.rrect(x, by + 74, 360, 6, 3, theme::line);
    g.rrect(x, by + 74, 360 * static_cast<float>(std::min(1.0, det_.progress())), 6, 3, kJf);
  }

  if (series) {
    // seasons
    float sx = kM;
    const float sy = 726;
    for (size_t i = 0; i < seasons_.size(); ++i) {
      const bool on = static_cast<int>(i) == season_;
      TextStyle st = head(19, on ? Color(22, 15, 11) : theme::dim, on);
      const float sw = g.measure(seasons_[i].name, st).x + 40;
      if (on) g.rrect(sx, sy, sw, 42, 21, theme::text);
      else ui::glass(g, sx, sy, sw, 42, 21, 1.f, 0.6f);
      if (zone_ == 1 && on) ui::focusRing(g, sx, sy, sw, 42, 21);
      g.text(seasons_[i].name, sx + 20, sy + (42 - g.measure(seasons_[i].name, st).y) / 2, st);
      sx += sw + 10;
    }
    // episodes
    const float ey = 792, ew = 300, eh = 169, gap = 20;
    const float tx = std::max(0.f, (ep_ - 1) * (ew + gap));
    epScroll_ += (tx - epScroll_) * std::min(1.f, dt * 10.f);
    g.clip(0, ey - 8, W, g.H() - ey + 8);
    for (size_t i = 0; i < episodes_.size(); ++i) {
      const float ex = kM - epScroll_ + i * (ew + gap);
      if (ex > W || ex + ew < 0) continue;
      const bool foc = zone_ == 2 && static_cast<int>(i) == ep_;
      drawTile(episodes_[i], ex, ey, ew, eh, true, foc, now);
      TextStyle lt = head(17, foc ? theme::text : theme::dim, foc);
      const std::string nm = std::to_string(episodes_[i].episodeNo) + ". " + episodes_[i].name;
      g.text(g.ellipsize(nm, lt, ew), ex, ey + eh + 10, lt);
    }
    g.unclip();
    if (episodes_.empty()) ui::spinner(g, kM + 16, ey + 60, 14, now);
  }
}

void JellyfinApp::drawSetup(double now, PadStyle) {
  Gfx& g = c_.gfx;
  const float W = g.W();
  g.vgrad(0, 0, W, g.H(), Color(24, 16, 30), theme::bg);
  g.glow(W / 2, 220, 700, 320, kJf.alpha(0.18f));
  drawTop(now, "");
  const float cw = 940, cx = (W - cw) / 2, cy = 150;
  const auto ch = setupChoices();
  const float rowH = 66;
  float ch_h = 190 + std::max<size_t>(ch.size(), 2) * rowH + 40;
  if (step_ == Step::QuickConnect) ch_h += 150;
  ui::glass(g, cx, cy, cw, ch_h, 28, 1.f, 0.78f);

  TextStyle title = head(40, theme::text);
  TextStyle sub = body(21, theme::dim);
  float y = cy + 44;
  auto line = [&](const std::string& s, TextStyle st) {
    st.wrap = cw - 120;
    Vec2 m = g.measure(s, st);
    g.text(s, cx + 60, y, st);
    y += m.y + 10;
  };
  switch (step_) {
    case Step::Servers:
      line("Connect to Jellyfin", title);
      line(discovering_ ? "Looking for servers on your network…" : found_.empty() ? "No servers found on your network. Enter an address instead." : "Choose your server.", sub);
      break;
    case Step::Probing: line("Connecting…", title); line(server_.empty() ? "" : server_, sub); break;
    case Step::QuickConnect:
      line("Sign in with Quick Connect", title);
      line("On another device open Jellyfin, then your profile, then Quick Connect, and enter this code.", sub);
      break;
    case Step::Password: line("Sign in", title); line("to " + (serverName_.empty() ? server_ : serverName_), sub); break;
    case Step::Signing: line("Signing in…", title); break;
  }
  y += 8;

  if (step_ == Step::QuickConnect) {
    // the code in large cells
    const std::string code = qc_.code.empty() ? "······" : qc_.code;
    TextStyle ct = head(64, theme::text);
    const float cell = 84, cg = 12;
    const float total = code.size() * cell + (code.size() - 1) * cg;
    float x0 = cx + (cw - total) / 2;
    for (size_t i = 0; i < code.size(); ++i) {
      ui::glass(g, x0 + i * (cell + cg), y, cell, 104, 16, 1.f, 0.95f);
      std::string d(1, code[i]);
      Vec2 m = g.measure(d, ct);
      g.text(d, x0 + i * (cell + cg) + (cell - m.x) / 2, y + (104 - m.y) / 2, ct);
    }
    y += 126;
    TextStyle w = body(19, theme::faint);
    w.align = Align::Center;
    g.text("Waiting for approval…", cx + cw / 2, y, w);
    ui::spinner(g, cx + cw / 2 - 150, y + 12, 9, now);
    y += 44;
  }
  if (step_ == Step::Probing || step_ == Step::Signing) {
    ui::spinner(g, cx + cw / 2, y + 40, 24, now);
    return;
  }

  for (size_t i = 0; i < ch.size(); ++i) {
    const bool foc = static_cast<int>(i) == setupSel_;
    const float ry = y + i * rowH;
    ui::glass(g, cx + 40, ry, cw - 80, rowH - 10, 30, 1.f, foc ? 0.98f : 0.55f);
    if (foc) ui::focusRing(g, cx + 40, ry, cw - 80, rowH - 10, 30);
    TextStyle rt = head(22, foc ? theme::text : theme::dim, foc);
    g.text(ch[i], cx + 72, ry + (rowH - 10 - g.measure(ch[i], rt).y) / 2, rt);
    if (step_ == Step::Servers && static_cast<int>(i) < static_cast<int>(found_.size())) {
      TextStyle at = body(17, theme::faint);
      const std::string a = found_[i].address;
      g.text(a, cx + cw - 72 - g.measure(a, at).x, ry + (rowH - 10 - g.measure(a, at).y) / 2, at);
    }
  }
  if (!setupError_.empty()) {
    TextStyle e = body(20, theme::err);
    e.align = Align::Center;
    e.wrap = cw - 120;
    g.text(setupError_, cx + cw / 2, cy + ch_h + 22, e);
  }
}

// ---- demo ------------------------------------------------------------------------------------------------------------------------

void JellyfinApp::loadDemo() {
  demo_ = true;
  JfSession s;
  s.server = "http://demo.local:8096";
  s.serverName = "Living Room";
  s.token = "demo";
  s.userId = "u1";
  s.userName = "Jamie";
  jf_.setSession(s);
  auto mk = [](const char* id, const char* name, const char* type, int year, int mins, const char* ov) {
    JfItem i;
    i.id = id;
    i.name = name;
    i.type = type;
    i.year = year;
    i.runtimeMin = mins;
    i.runtimeTicks = int64_t(mins) * 600000000LL;
    i.overview = ov;
    i.officialRating = "PG-13";
    i.communityRating = 7.8;
    i.genres = {"Drama", "Sci-Fi"};
    i.folder = std::string(type) == "Series" || std::string(type) == "CollectionFolder";
    return i;
  };
  auto ep = [&](const char* id, const char* name, int s_, int e, int mins, double prog, bool played) {
    JfItem i = mk(id, name, "Episode", 2021, mins, "A tense hour aboard the station as the crew finds something that was not on the manifest.");
    i.seriesName = "Signal Fire";
    i.seriesId = "s1";
    i.seasonNo = s_;
    i.episodeNo = e;
    i.positionTicks = static_cast<int64_t>(i.runtimeTicks * prog);
    i.played = played;
    return i;
  };
  const char* long1 = "A salvage pilot finds a derelict transmitting a message only she can understand, and the only people who can help are the ones who want her silenced.";
  Row cont;
  cont.title = "Continue watching";
  cont.landscape = true;
  cont.items = {ep("e1", "The Manifest", 1, 3, 48, 0.42, false), mk("m1", "Ashfall", "Movie", 2019, 124, long1),
                ep("e2", "Cold Open", 2, 1, 51, 0.12, false), mk("m2", "The Long Quiet", "Movie", 2022, 109, long1)};
  cont.items[1].positionTicks = cont.items[1].runtimeTicks / 3;
  cont.items[3].positionTicks = cont.items[3].runtimeTicks * 4 / 5;
  Row nxt;
  nxt.title = "Next up";
  nxt.landscape = true;
  nxt.items = {ep("e3", "Crosswind", 1, 4, 47, 0, false), ep("e4", "Low Orbit", 3, 2, 52, 0, false), ep("e5", "Ember", 1, 8, 44, 0, false)};
  Row libs;
  libs.title = "Libraries";
  libs.landscape = true;
  libs.items = {mk("l1", "Movies", "CollectionFolder", 0, 0, ""), mk("l2", "Shows", "CollectionFolder", 0, 0, ""), mk("l3", "Documentaries", "CollectionFolder", 0, 0, "")};
  Row lat;
  lat.title = "Latest in Movies";
  lat.items = {mk("m3", "Tidewater", "Movie", 2024, 118, long1), mk("m4", "Harbor Light", "Movie", 2023, 96, long1), mk("m5", "Paper Moon", "Movie", 2021, 132, long1),
               mk("m6", "Nine Lives", "Movie", 2020, 101, long1), mk("m7", "Switchback", "Movie", 2018, 107, long1), mk("m8", "Kestrel", "Movie", 2024, 89, long1),
               mk("m9", "The Understudy", "Movie", 2022, 115, long1)};
  lat.items[1].played = true;
  rows_ = {cont, nxt, libs, lat};
  for (auto& r : rows_) r.loaded = true;
  row_ = 0;
  mode_ = Mode::Home;
  demoSeasons_ = seasons_ = {mk("se1", "Season 1", "Season", 2021, 0, ""), mk("se2", "Season 2", "Season", 2022, 0, ""), mk("se3", "Season 3", "Season", 2023, 0, "")};
  seasons_[0].unplayedCount = 5;
  demoEpisodes_ = episodes_ = {ep("e1", "The Manifest", 1, 1, 48, 0, true), ep("e2", "Cold Open", 1, 2, 51, 0, true), ep("e6", "Crosswind", 1, 3, 47, 0.42, false),
               ep("e7", "Low Orbit", 1, 4, 52, 0, false), ep("e8", "Ember", 1, 5, 44, 0, false), ep("e9", "Static", 1, 6, 50, 0, false)};
}

void JellyfinApp::demoScreen(const std::string& v) {
  if (v == "setup" || v == "code" || v == "password") {
    mode_ = Mode::Setup;
    serverName_ = "Living Room";
    server_ = "http://192.168.1.20:8096";
    found_ = {{"Living Room", "http://192.168.1.20:8096", "a"}, {"Basement NAS", "http://192.168.1.31:8096", "b"}};
    step_ = v == "code" ? Step::QuickConnect : v == "password" ? Step::Password : Step::Servers;
    qc_ = {"secret", "482917"};
    user_ = "jamie";
    setupSel_ = 0;
  } else if (v == "detail") {
    JfItem s;
    s.id = "s1";
    s.name = "Signal Fire";
    s.type = "Series";
    s.year = 2021;
    s.folder = true;
    s.officialRating = "TV-MA";
    s.communityRating = 8.4;
    s.genres = {"Drama", "Sci-Fi"};
    s.overview = "When a deep-space relay station goes dark, a salvage crew is sent to find out why. What they find on board was never in the manifest.";
    openDetail(s);
    zone_ = 2;
    ep_ = 2;
  } else if (v == "movie") {
    if (!rows_.empty() && rows_[0].items.size() > 1) openDetail(rows_[0].items[1]);
  } else if (v == "grid") {
    openGrid("Movies", "l1");
    grid_ = rows_[3].items;
    for (int k = 0; k < 2; ++k)
      for (auto it : rows_[3].items) { it.id += "x" + std::to_string(k); grid_.push_back(it); }
    gridTotal_ = static_cast<int>(grid_.size());
    gSel_ = 9;
  }
}

}  // namespace vos
