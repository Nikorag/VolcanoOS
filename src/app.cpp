#include "app.hpp"

#include <SDL_image.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "http.hpp"
#include "json.hpp"
#include "tokenstore.hpp"
#include "widgets.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace vos {

using nlohmann::json;

namespace {

std::string hostName() {
#ifdef _WIN32
  const char* n = std::getenv("COMPUTERNAME");
  return n ? n : "Windows PC";
#else
  char buf[256] = {0};
  if (gethostname(buf, sizeof buf - 1) == 0 && buf[0]) {
    std::string s = buf;
    if (auto dot = s.find('.'); dot != std::string::npos) s.resize(dot);
    return s;
  }
  return "Unknown device";
#endif
}

}  // namespace

App::App(Options o) : opt_(std::move(o)) {}
App::~App() = default;

// ---- lifecycle ----------------------------------------------------------------

void App::loadSettings() {
  std::string text;
  if (readFile(dataDir() / "settings.json", text)) {
    json j = json::parse(text, nullptr, false);
    if (j.is_object()) fullscreen_ = j.value("fullscreen", true);
  }
  if (opt_.windowed) fullscreen_ = false;
  if (opt_.fullscreen) fullscreen_ = true;
  if (!opt_.shotPath.empty() && !opt_.fullscreen) fullscreen_ = false;
}

void App::saveSettings() {
  if (opt_.demo) return;
  json j = {{"fullscreen", fullscreen_}};
  writeFileAtomic(dataDir() / "settings.json", j.dump(2));
}

bool App::initWindow() {
  Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
  if (fullscreen_) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  win_ = SDL_CreateWindow("Zonto Volcano", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt_.width, opt_.height, flags);
  if (!win_) {
    logf("window: %s", SDL_GetError());
    return false;
  }
  ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
  if (!ren_) {
    logf("renderer: %s", SDL_GetError());
    return false;
  }
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
  return gfx_.init(win_, ren_);
}

void App::setFullscreen(bool on) {
  fullscreen_ = on;
  SDL_SetWindowFullscreen(win_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  saveSettings();
}

int App::run() {
  httpGlobalInit();
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");  // instant, no Spaces animation
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    logf("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  if (TTF_Init() != 0) {
    logf("TTF_Init: %s", TTF_GetError());
    return 1;
  }
  IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
  SDL_DisableScreenSaver();  // a launcher on a TV shouldn't blank mid-menu

  pool_ = std::make_unique<Pool>(4);
  installer_ = std::make_unique<Installer>(api_, main_);
  installer_->setDemo(opt_.demo);
  loadSettings();
  if (!initWindow()) return 1;
  art_ = std::make_unique<Art>(gfx_, *pool_, main_);
  Pages::Hooks hooks;
  hooks.toast = [this](const std::string& t, Color c) { toast(t, c, 5); };
  hooks.text = [this](const std::string& title, const std::string& init, size_t max,
                      std::function<void(const std::string&)> done) {
    osk_.show(title, init, max, std::move(done));
    SDL_StartTextInput();
  };
  hooks.confirm = [this](const std::string& t, const std::string& m, const std::string& ok, std::function<void()> fn) {
    openConfirm(t, m, ok, std::move(fn));
  };
  hooks.unauthorized = [this] {
    signOut();
    signInError_ = "YOUR SIGN-IN EXPIRED. SIGN IN AGAIN.";
  };
  hooks.tick = [this] { input_.tick(0.1f, 12); };
  pages_ = std::make_unique<Pages>(gfx_, api_, *pool_, main_, std::move(hooks));
  bg_.init();
  input_.init();
  web_ = createWebView();
  web_->setMessageHandler([this](const std::string& json) {
    GameEvent e;
    if (GameBridge::parse(json, e)) onGameEvent(e);
  });
  initOverlay();
  if (fullscreen_) SDL_ShowCursor(SDL_DISABLE);

  installer_->onFinished = [this](const std::string& slug, bool ok, bool cancelled, const std::string& msg) {
    std::string name = slug;
    for (auto& g : games_)
      if (g.slug == slug) name = g.name;
    if (ok) {
      toast(upper(name) + " IS READY TO PLAY", theme::ok);
      input_.tick(0.5f, 80);
      syncDevice();
    } else if (cancelled) {
      toast("INSTALL CANCELLED", theme::dim);
    } else {
      toast(upper(name) + ": " + msg, theme::err, 7);
    }
    rebuildView();
  };

  startTime_ = nowSeconds();
  if (!opt_.demo) installer_->scan();
  boot();

  double last = nowSeconds();
  while (running_) {
    double now = nowSeconds();
    float dt = static_cast<float>(std::min(0.1, now - last));
    last = now;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running_ = false; break;
        case SDL_MOUSEMOTION:
          lastMouseMove_ = now;
          if (fullscreen_) SDL_ShowCursor(SDL_ENABLE);
          break;
        case SDL_MOUSEWHEEL:
          input_.push(e.wheel.y > 0 || e.wheel.x < 0 ? Action::Left : Action::Right);
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (screen_ == Screen::Home && !menu_.open && sess_.kind == SessionKind::None && e.button.button == SDL_BUTTON_LEFT) {
            // Map window pixels -> UI units and hit-test the shelf.
            int ww, wh, ow, oh;
            SDL_GetWindowSize(win_, &ww, &wh);
            SDL_GetRendererOutputSize(ren_, &ow, &oh);
            float px = e.button.x * (static_cast<float>(ow) / ww) / gfx_.scale();
            float py = e.button.y * (static_cast<float>(oh) / wh) / gfx_.scale();
            for (auto& h : hits_) {
              if (px >= h.x && px <= h.x + h.w && py >= h.y && py <= h.y + h.h) {
                if (h.idx == sel_) input_.push(Action::Accept);
                else sel_ = h.idx;
                break;
              }
            }
          }
          break;
        case SDL_TEXTINPUT:
          if (osk_.isOpen()) osk_.typeText(e.text.text);
          break;
        case SDL_KEYDOWN:
          if (osk_.isOpen()) {
            // Physical keyboard types straight into the on-screen keyboard.
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_BACKSPACE) { osk_.backspace(); continue; }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { osk_.submit(); SDL_StopTextInput(); continue; }
            if (k == SDLK_ESCAPE) { osk_.cancel(); SDL_StopTextInput(); continue; }
            if (k < 0x80 && k >= 32 && !(e.key.keysym.mod & KMOD_CTRL)) continue;  // text arrives via SDL_TEXTINPUT
          }
          if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
            setFullscreen(!fullscreen_);
            continue;
          }
          input_.handle(e);
          break;
        default: input_.handle(e); break;
      }
    }
    if (fullscreen_ && now - lastMouseMove_ > 2.5) SDL_ShowCursor(SDL_DISABLE);

    // Screenshot-mode scripting: one synthetic input every few frames.
    if (opt_.demo && scriptPos_ < opt_.script.size() && frameNo_ > 20 && frameNo_ % 6 == 0) input_.push(opt_.script[scriptPos_++]);

    input_.update(now);
    Action a;
    while (input_.poll(a)) handleAction(a);
    main_.drain();
    pollSession(now);

    frame(now, dt);
    ++frameNo_;

    if (!opt_.shotPath.empty() && !shotTaken_ && static_cast<int>(frameNo_) >= opt_.shotFrame) {
      saveShot();
      shotTaken_ = true;
      running_ = false;
    }
    // A running game doesn't need us at 60 fps.
    if (sess_.kind != SessionKind::None) SDL_Delay((panel_ || !gameToasts_.empty()) ? 30 : 90);
  }

  // ---- teardown ----------------------------------------------------------------
  signin_.cancel();
  bridge_.reset();
  if (overlayWin_) overlayWin_->show(false);
  if (web_ && web_->isOpen()) web_->close();
  if (sess_.kind != SessionKind::None && api_.hasToken() && !opt_.demo) {
    api_.presence(std::nullopt);  // don't leave "now playing" behind
  }
  installer_.reset();
  pages_.reset();
  shutdownOverlay();
  art_.reset();
  gfx_.shutdown();
  input_.shutdown();
  pool_.reset();
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
  IMG_Quit();
  TTF_Quit();
  SDL_Quit();
  httpGlobalCleanup();
  return 0;
}

void App::saveShot() {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(ren_, &w, &h);
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return;
  if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
    if (IMG_SavePNG(s, opt_.shotPath.c_str()) == 0) logf("screenshot written: %s (%dx%d)", opt_.shotPath.c_str(), w, h);
    else logf("screenshot failed: %s", IMG_GetError());
  }
  SDL_FreeSurface(s);
}

// ---- account ------------------------------------------------------------------

void App::boot() {
  screenSince_ = nowSeconds();
  if (opt_.demo) {
    loadDemo();
    screen_ = opt_.screen == "signin" ? Screen::SignIn : Screen::Home;
    pages_->loadDemo();
    if (opt_.screen == "system") openSystemMenu();
    if (opt_.screen == "achievements") openPage(PageTab::Achievements);
    if (opt_.screen == "social") openPage(PageTab::Social);
    if (opt_.screen == "profile") openPage(PageTab::Profile);
    return;
  }
  auto tok = tokenstore::load();
  if (!tok) {
    screen_ = Screen::SignIn;
    return;
  }
  api_.setToken(*tok);
  bootStatus_ = "CONNECTING";
  resolveAccount();
}

void App::resolveAccount() {
  bootStatus_ = "CONNECTING";
  if (screen_ != Screen::Home) screen_ = Screen::Boot;
  signInBusy_ = true;
  pool_->run<Result<Player>>(
      main_, [this] { return api_.me(); },
      [this](Result<Player> r) {
        signInBusy_ = false;
        if (r.ok) {
          player_ = r.value;
          offline_ = false;
          json j = {{"id", player_.id}, {"gamertag", player_.gamertag}, {"level", player_.level}};
          writeFileAtomic(dataDir() / "player.json", j.dump());
          screen_ = Screen::Home;
          screenSince_ = nowSeconds();
          loadCachedLibrary();
          refreshLibrary(true);
        } else if (r.unauthorized()) {
          tokenstore::clear();
          api_.setToken("");
          signInError_ = "YOUR SIGN-IN EXPIRED. SIGN IN AGAIN.";
          screen_ = Screen::SignIn;
          screenSince_ = nowSeconds();
        } else if (r.offline()) {
          std::string text;
          json j;
          if (readFile(dataDir() / "player.json", text)) j = json::parse(text, nullptr, false);
          if (j.is_object() && j.contains("gamertag")) {
            player_.id = j.value("id", int64_t(0));
            player_.gamertag = j.value("gamertag", "");
            player_.level = j.value("level", 0);
            offline_ = true;
            screen_ = Screen::Home;
            screenSince_ = nowSeconds();
            loadCachedLibrary();
            toast("OFFLINE. SHOWING YOUR LAST LIBRARY.", theme::warn, 6);
          } else {
            signInError_ = "CAN'T REACH ZONTO. CHECK YOUR CONNECTION.";
            screen_ = Screen::SignIn;
            screenSince_ = nowSeconds();
          }
        } else {
          signInError_ = upper(r.error);
          screen_ = Screen::SignIn;
          screenSince_ = nowSeconds();
        }
      });
}

void App::startSignIn() {
  signInError_.clear();
  signin_.start(api_.base());
}

void App::onSignedIn(const std::string& token) {
  tokenstore::save(token);
  api_.setToken(token);
  resolveAccount();
}

void App::signOut() {
  if (web_ && web_->isOpen()) web_->close();
  tokenstore::clear();
  api_.setToken("");
  std::error_code ec;
  fs::remove(dataDir() / "player.json", ec);
  fs::remove(dataDir() / "library.json", ec);
  games_.clear();
  view_.clear();
  if (pages_) pages_->reset();
  osk_.cancel();
  player_ = {};
  screen_ = Screen::SignIn;
  screenSince_ = nowSeconds();
  signInError_.clear();
}

// ---- library --------------------------------------------------------------------

void App::loadCachedLibrary() {
  std::string text;
  if (readFile(dataDir() / "library.json", text)) applyLibrary(Api::parseLibrary(text));
}

void App::refreshLibrary(bool quiet) {
  if (opt_.demo) {
    if (!quiet) toast("LIBRARY UP TO DATE", theme::dim, 2.5);
    return;
  }
  if (loadingLibrary_ || !api_.hasToken()) return;
  loadingLibrary_ = true;
  using R = std::pair<Result<std::vector<GameEntry>>, std::string>;
  pool_->run<R>(
      main_,
      [this] {
        std::string raw;
        auto r = api_.library(&raw);
        return R{std::move(r), std::move(raw)};
      },
      [this, quiet](R r) {
        loadingLibrary_ = false;
        auto& res = r.first;
        if (res.ok) {
          offline_ = false;
          writeFileAtomic(dataDir() / "library.json", r.second);
          applyLibrary(std::move(res.value));
          if (!quiet) toast("LIBRARY UP TO DATE", theme::dim, 2.5);
        } else if (res.unauthorized()) {
          signOut();
          signInError_ = "YOUR SIGN-IN EXPIRED. SIGN IN AGAIN.";
        } else if (res.offline()) {
          offline_ = true;
          toast("OFFLINE. SHOWING YOUR LAST LIBRARY.", theme::warn, 5);
        } else {
          toast(upper(res.error), theme::err, 6);
        }
      });
}

void App::applyLibrary(std::vector<GameEntry> g) {
  std::string keep;
  if (const GameEntry* s = selected()) keep = s->slug;
  // Recently played first, then never-played alphabetically.
  std::stable_sort(g.begin(), g.end(), [](const GameEntry& a, const GameEntry& b) {
    if (a.lastPlayedAt != b.lastPlayedAt) return a.lastPlayedAt > b.lastPlayedAt;
    return upper(a.name) < upper(b.name);
  });
  games_ = std::move(g);
  rebuildView();
  if (!keep.empty())
    for (size_t i = 0; i < view_.size(); ++i)
      if (games_[view_[i]].slug == keep) sel_ = static_cast<int>(i);
  syncDevice();
}

void App::rebuildView() {
  std::string keep;
  if (const GameEntry* s = selected()) keep = s->slug;
  view_.clear();
  for (size_t i = 0; i < games_.size(); ++i) {
    const GameEntry& g = games_[i];
    bool ok = true;
    if (filter_ == Filter::Installed) ok = installer_->installed(g.slug) != nullptr;
    else if (filter_ == Filter::Web) ok = g.isWeb();
    if (ok) view_.push_back(static_cast<int>(i));
  }
  sel_ = std::min(sel_, std::max(0, static_cast<int>(view_.size()) - 1));
  if (!keep.empty())
    for (size_t i = 0; i < view_.size(); ++i)
      if (games_[view_[i]].slug == keep) sel_ = static_cast<int>(i);
}

const GameEntry* App::selected() const {
  if (view_.empty() || sel_ < 0 || sel_ >= static_cast<int>(view_.size())) return nullptr;
  return &games_[view_[sel_]];
}

void App::syncDevice() {
  if (opt_.demo || !api_.hasToken()) return;
  fs::path keyFile = dataDir() / "device.json";
  std::string key;
  std::string text;
  if (readFile(keyFile, text)) {
    json j = json::parse(text, nullptr, false);
    if (j.is_object()) key = j.value("deviceKey", "");
  }
  if (key.empty()) {
    key = randomToken(18);
    writeFileAtomic(keyFile, json{{"deviceKey", key}}.dump());
  }
  auto [platform, arch] = buildTarget();
  json installs = json::array();
  for (auto& [slug, version] : installer_->installs()) installs.push_back({{"slug", slug}, {"version", version}});
  json body = {{"deviceKey", key}, {"name", hostName()}, {"platform", platform},
               {"arch", arch},     {"launcherVersion", VOS_VERSION}, {"installs", installs}};
  pool_->submit([this, b = body.dump()] {
    auto r = api_.syncDevice(b);
    if (!r.ok) logf("device sync: %s", r.error.c_str());
  });
}

// ---- demo fixtures ----------------------------------------------------------------

void App::loadDemo() {
  auto mk = [](const char* slug, const char* name, const char* tags, const char* blurb, bool build, const char* ver,
               uint64_t size, bool web, int mins, int au, int at, const char* last) {
    GameEntry g;
    g.slug = slug;
    g.name = name;
    g.tags = tags;
    g.blurb = blurb;
    g.hasBuild = build;
    g.build.version = ver;
    g.build.exePath = build ? "game" : "";
    g.build.sizeBytes = size;
    if (web) g.playUrl = std::string("https://example.invalid/") + slug;
    g.minutesPlayed = mins;
    g.achUnlocked = au;
    g.achTotal = at;
    g.lastPlayedAt = last;
    return g;
  };
  std::vector<GameEntry> g = {
      mk("carve-the-fate", "Carve The Fate", "ACTION · ROGUELIKE",
         "Hack through a drowned kingdom one decision at a time. Every blade you forge rewrites the tide, and every death carves a little more of the map.",
         true, "1.4.0", 1288490188ull, false, 1830, 7, 24, "2026-10-28T20:11:00Z"),
      mk("peggy", "Peggy!", "PUZZLE · COZY",
         "A pinball-flavoured puzzle about a very determined hedgehog and a very large pile of buttons.", true, "2.1.0",
         412000000ull, true, 640, 12, 12, "2026-10-26T18:00:00Z"),
      mk("cycic-citizen", "Cycic Citizen", "SIM · CITY",
         "Build a bike-first city from a single cobbled road. Keep everyone moving, dry and mildly smug.", true, "0.9.2",
         905000000ull, false, 95, 2, 18, "2026-10-20T09:30:00Z"),
      mk("fruit-heist", "Fruit Heist", "ARCADE · CO-OP",
         "Four thieves, one orchard, zero chill. Plan the job, grab the pears, and get out before the farmer wakes.", true,
         "1.0.3", 220000000ull, true, 0, 0, 9, ""),
      mk("hungry-birds", "Hungry Birds", "CASUAL · FREE",
         "Feed the flock before the sun sets. Playable instantly in your browser.", false, "", 0, true, 210, 4, 8,
         "2026-10-15T12:00:00Z"),
      mk("pine", "Pine", "ADVENTURE · NARRATIVE",
         "A quiet walk through a forest that remembers you. Short, strange and worth the whole afternoon.", true, "1.0.0",
         160000000ull, false, 45, 1, 6, "2026-10-01T22:45:00Z"),
      mk("signal-decay", "Signal Decay", "SURVIVAL · HORROR",
         "Keep the relay alive. Whatever is on the other end is getting louder.", true, "1.4.0", 2100000000ull, false, 0,
         0, 20, ""),
      mk("dust-choir", "Dust Choir", "MUSIC · EXPLORATION",
         "Sing the desert awake. A game about resonance, patience, and very large instruments.", false, "", 0, false, 0,
         0, 14, ""),
  };
  // Real public artwork, so screenshots exercise text legibility over busy images.
  const std::string art = "https://storage.googleapis.com/zontohub-game-art/games/";
  auto setArt = [&](const char* slug, const char* logo, const char* hero) {
    for (auto& e : g)
      if (e.slug == slug) {
        if (*logo) e.logoUrl = art + logo;
        if (*hero) e.heroUrl = art + hero;
      }
  };
  setArt("carve-the-fate", "carve-the-fate/hd-carve-art.jpg", "carve-the-fate/hd-carve-hero.jpg");
  setArt("peggy", "peggy/hd-peggy-art.jpg", "peggy/hd-peggy-hero.jpg");
  setArt("cycic-citizen", "cycic-citizen/hd-cycic-art.jpg", "cycic-citizen/hd-cycic-hero.jpg");
  setArt("hungry-birds", "hungrybirds/hd-birds-art.jpg", "hungrybirds/hd-birds-hero.jpg");
  setArt("pine", "pine/hd-pine-art.jpg", "");
  applyLibrary(std::move(g));
  player_.gamertag = "Nikorag";
  player_.level = 14;
  if (auto* i = installer_.get()) {
    Installed a;
    a.slug = "carve-the-fate"; a.version = "1.3.0"; a.exePath = "game";
    i->injectInstalled(a);  // update available
    Installed b;
    b.slug = "peggy"; b.version = "2.1.0"; b.exePath = "game";
    i->injectInstalled(b);
    Installed c;
    c.slug = "pine"; c.version = "1.0.0"; c.exePath = "game";
    i->injectInstalled(c);
  }
  rebuildView();
}

// ---- toasts ------------------------------------------------------------------------

void App::toast(const std::string& text, Color c, double secs) {
  double now = nowSeconds();
  toasts_.push_back({text, c, now, now + secs});
  if (toasts_.size() > 3) toasts_.pop_front();
}

// ---- game actions ---------------------------------------------------------------------

App::Primary App::primaryFor(const GameEntry& g) const {
  if (sess_.kind != SessionKind::None && sess_.slug == g.slug) return {Kind::Running, "NOW PLAYING", false};
  if (auto j = installer_->job(g.slug); j && j->active()) return {Kind::Installing, "INSTALLING", false};
  const Installed* inst = installer_->installed(g.slug);
  if (g.hasBuild) {
    if (!inst) return {Kind::Install, "INSTALL", !offline_};
    if (inst->version != g.build.version && !offline_) return {Kind::Update, "UPDATE", true};
    return {Kind::Play, "PLAY", true};
  }
  if (inst) return {Kind::Play, "PLAY", true};
  if (g.isWeb()) return {Kind::PlayWeb, "PLAY IN BROWSER", !offline_};
  return {Kind::Unavailable, "NOT AVAILABLE ON THIS DEVICE", false};
}

void App::doPrimary(const GameEntry& g) {
  if (launching_) return;
  Primary p = primaryFor(g);
  if (!p.enabled) {
    if (p.kind == Kind::Unavailable) toast("NO BUILD OF " + upper(g.name) + " FOR THIS DEVICE YET", theme::warn);
    else if (offline_) toast("YOU'RE OFFLINE", theme::warn);
    input_.tick(0.15f, 20);
    return;
  }
  if (sess_.kind != SessionKind::None) {
    toast("FINISH YOUR CURRENT SESSION FIRST", theme::warn);
    return;
  }
  switch (p.kind) {
    case Kind::Install:
    case Kind::Update: install(g); break;
    case Kind::Play: launchNative(g); break;
    case Kind::PlayWeb: launchWeb(g); break;
    default: break;
  }
}

void App::install(const GameEntry& g) {
  if (!installer_->start(g.slug, g.name, g.build)) return;
  toast((installer_->installed(g.slug) ? "UPDATING " : "INSTALLING ") + upper(g.name), theme::ember, 3);
}

void App::launchNative(const GameEntry& g) {
  const Installed* inst = installer_->installed(g.slug);
  if (!inst) return;
  if (opt_.demo && opt_.fakeGame.empty()) {
    toast("DEMO: WOULD LAUNCH " + upper(g.name), theme::ok);
    return;
  }
  // Hand the game a private, loopback-only channel to the overlay.
  bridge_ = std::make_unique<GameBridge>(main_);
  GameProcess::Env env;
  if (bridge_->start([this](GameEvent e) { onGameEvent(e); })) {
    env = {{"ZONTO_LAUNCHER_URL", "http://127.0.0.1:" + std::to_string(bridge_->port())},
           {"ZONTO_LAUNCHER_TOKEN", bridge_->token()},
           {"ZONTO_GAME_SLUG", g.slug}};
  } else {
    bridge_.reset();
  }
  std::string err;
  fs::path log = dataDir() / "logs" / (slugify(g.slug) + ".log");
  fs::path exe = opt_.demo ? fs::path(opt_.fakeGame) : inst->executable();
  fs::path wd = opt_.demo ? dataDir() : inst->versionDir();
  if (!proc_.spawn(exe, wd, opt_.demo ? "" : inst->launchArgs, log, err, env)) {
    bridge_.reset();
    toast(upper(err), theme::err, 7);
    return;
  }
  sess_ = Session{SessionKind::Native, g.slug, g.name, nowSeconds(), nowSeconds(), g.selfReportsPlaytime};
  beginSession();
  input_.tick(0.4f, 60);
  if (opt_.demo) return;
  // Record the launch (sets last-played) and present as "now playing". An
  // offline player may still play what's on disk, so failures here are quiet.
  pool_->submit([this, slug = g.slug] {
    api_.recordLaunch(slug);
    api_.presence(slug);
  });
}

void App::launchWeb(const GameEntry& g) {
  if (opt_.demo) {
    toast("DEMO: WOULD OPEN " + upper(g.name) + " IN THE BROWSER RENDERER", theme::ok);
    return;
  }
  launching_ = true;
  pool_->run<Result<LaunchInfo>>(
      main_, [this, slug = g.slug] { return api_.recordLaunch(slug); },
      [this, g](Result<LaunchInfo> r) {
        launching_ = false;
        if (!r.ok) {
          toast(upper(r.error), theme::err, 6);
          return;
        }
        std::string url = r.value.launchUrl.empty() ? g.playUrl : r.value.launchUrl;
        std::string err;
        if (!web_->open(url, g.name, fullscreen_, err)) {
          toast(upper(err), theme::err, 7);
          return;
        }
        sess_ = Session{SessionKind::WebGame, g.slug, g.name, nowSeconds(), nowSeconds(), true};
        beginSession();
        pool_->submit([this, slug = g.slug] { api_.presence(slug); });
      });
}

void App::pollSession(double now) {
  if (sess_.kind == SessionKind::None) return;
  if (sess_.kind == SessionKind::Native) {
    int code = 0;
    if (proc_.poll(code)) {
      endSession(code != 0, code);
      return;
    }
  } else if (!web_->isOpen()) {
    endSession(false, 0);
    return;
  }
  if (now - sess_.lastBeat >= 60) {
    sess_.lastBeat = now;
    pool_->submit([this, slug = sess_.slug] { api_.presence(slug); });
  }
  pollInvites(now);
}

void App::endSession(bool crashed, int code) {
  Session s = sess_;
  sess_ = Session{};
  bridge_.reset();
  closePanel();
  gameToasts_.clear();
  gameStatus_.clear();
  if (overlayWin_) overlayWin_->show(false);
  SDL_RaiseWindow(win_);

  int minutes = static_cast<int>((nowSeconds() - s.start) / 60);
  pool_->submit([this, s, minutes] {
    api_.presence(std::nullopt);  // explicit null: stopped
    // Wall-clock minutes only when the game doesn't report its own.
    if (s.kind == SessionKind::Native && !s.selfReports && minutes >= 1) api_.reportMinutes(s.slug, minutes);
  });
  if (s.kind == SessionKind::Native && minutes >= 1)
    for (auto& g : games_)
      if (g.slug == s.slug && !g.selfReportsPlaytime) g.minutesPlayed += minutes;
  if (crashed) toast(upper(s.name) + " EXITED WITH CODE " + std::to_string(code), theme::warn, 6);
  else toast("WELCOME BACK. " + std::to_string(std::max(0, minutes)) + " MIN PLAYED", theme::dim, 3);
  input_.tick(0.3f, 50);
}

// ---- input --------------------------------------------------------------------------

void App::handleAction(Action a) {
  // The on-screen keyboard sits above everything else.
  if (osk_.isOpen()) {
    osk_.handle(a);
    if (!osk_.isOpen()) SDL_StopTextInput();
    return;
  }
  // Menus own input while open.
  if (menu_.open) {
    int n = static_cast<int>(menu_.items.size());
    if (a == Action::Up || a == Action::Down) {
      menu_.sel = (menu_.sel + (a == Action::Down ? 1 : n - 1)) % n;
      input_.tick(0.12f, 15);
    } else if (a == Action::Accept) {
      const MenuItem& it = menu_.items[menu_.sel];
      if (it.enabled) {
        int id = it.id;
        auto cb = menu_.onPick;
        closeMenu();
        if (cb) cb(id);
        input_.tick(0.2f, 25);
      } else {
        input_.tick(0.15f, 20);
      }
    } else if (a == Action::Back || a == Action::Start || a == Action::Home) {
      closeMenu();
    }
    return;
  }

  // A game is running: Home opens the Volcano OS menu over it.
  if (sess_.kind != SessionKind::None) {
    if (!panel_) {
      if (a == Action::Home) openPanel();
      return;
    }
    int n = sess_.kind == SessionKind::Native ? 3 : 2;
    if (a == Action::Up || a == Action::Down) {
      panelSel_ = (panelSel_ + (a == Action::Down ? 1 : n - 1)) % n;
      quitArmed_ = false;
      input_.tick(0.1f, 12);
    } else if (a == Action::Accept) {
      panelAction();
    } else if (a == Action::Back || a == Action::Home || a == Action::Start) {
      closePanel();
    }
    return;
  }

  if (screen_ == Screen::SignIn) {
    if (a == Action::Accept) {
      if (signin_.state() == SignInFlow::State::Waiting || signInBusy_) return;
      if (api_.hasToken()) resolveAccount();  // offline retry
      else startSignIn();
    } else if (a == Action::Back) {
      if (signin_.state() == SignInFlow::State::Waiting) signin_.cancel();
    } else if (a == Action::Start || a == Action::Home) {
      openSystemMenu();
    }
    return;
  }
  if (screen_ == Screen::Pages) {
    if (pages_->handle(a)) {
      screen_ = Screen::Home;
      screenSince_ = nowSeconds();
    }
    return;
  }
  if (screen_ != Screen::Home) return;

  int n = static_cast<int>(view_.size());
  switch (a) {
    case Action::Left:
      if (sel_ > 0) { --sel_; input_.tick(0.1f, 12); }
      break;
    case Action::Right:
      if (sel_ + 1 < n) { ++sel_; input_.tick(0.1f, 12); }
      break;
    case Action::Up:
    case Action::Down:
      break;
    case Action::LB:
    case Action::RB: {
      int f = static_cast<int>(filter_);
      f = (f + (a == Action::RB ? 1 : 2)) % 3;
      filter_ = static_cast<Filter>(f);
      sel_ = 0;
      cam_ = 0;
      rebuildView();
      input_.tick(0.15f, 20);
      break;
    }
    case Action::Accept:
      if (const GameEntry* g = selected()) doPrimary(*g);
      break;
    case Action::X:
      if (selected()) openGameMenu();
      break;
    case Action::Y: refreshLibrary(false); break;
    case Action::Back:
      if (const GameEntry* g = selected())
        if (auto j = installer_->job(g->slug); j && j->active()) installer_->cancel(g->slug);
      break;
    case Action::Start:
    case Action::Home:
    case Action::Select: openSystemMenu(); break;
    default: break;
  }
}

// ---- menus ---------------------------------------------------------------------------

void App::openConfirm(const std::string& title, const std::string& msg, const std::string& okLabel,
                      std::function<void()> onOk) {
  menu_ = Menu{};
  menu_.open = true;
  menu_.title = title;
  menu_.message = msg;
  menu_.items = {{"CANCEL", "", 0, true, false}, {okLabel, "", 1, true, true}};
  menu_.sel = 0;  // default to the safe choice
  menu_.openedAt = nowSeconds();
  menu_.onPick = [onOk](int id) {
    if (id == 1) onOk();
  };
}

void App::openGameMenu() {
  const GameEntry* g = selected();
  if (!g) return;
  std::string slug = g->slug, name = g->name;
  const Installed* inst = installer_->installed(slug);
  auto job = installer_->job(slug);
  bool installing = job && job->active();

  menu_ = Menu{};
  menu_.open = true;
  menu_.title = upper(name);
  menu_.openedAt = nowSeconds();
  enum { PlayWebId = 1, Cancel, Reinstall, Uninstall, Folder };
  if (g->isWeb() && g->hasBuild) menu_.items.push_back({"PLAY IN BROWSER", "Run the web version instead", PlayWebId, !offline_ && !installing});
  if (installing) menu_.items.push_back({"CANCEL INSTALL", "Keeps the download so you can resume", Cancel});
  if (inst && !installing) {
    if (g->hasBuild) menu_.items.push_back({"REINSTALL", "Fresh copy of v" + g->build.version, Reinstall, !offline_});
    menu_.items.push_back({"SHOW INSTALL FOLDER", "", Folder});
    menu_.items.push_back({"UNINSTALL", "Removes the game files. Saves are kept.", Uninstall, true, true});
  }
  menu_.sel = 0;
  menu_.onPick = [this, slug, name](int id) {
    const GameEntry* g = nullptr;
    for (auto& x : games_)
      if (x.slug == slug) g = &x;
    if (!g) return;
    switch (id) {
      case PlayWebId:
        if (sess_.kind == SessionKind::None) launchWeb(*g);
        break;
      case Cancel: installer_->cancel(slug); break;
      case Reinstall:
        installer_->uninstall(slug);
        rebuildView();
        install(*g);
        break;
      case Folder: {
        if (const Installed* i = installer_->installed(slug)) {
          std::string url = "file://" + i->root().string();
          SDL_OpenURL(url.c_str());
        }
        break;
      }
      case Uninstall:
        openConfirm("UNINSTALL " + upper(name) + "?", "The game files will be removed from this device. Your saves and progress stay with your account.", "UNINSTALL",
                    [this, slug, name] {
                      if (installer_->uninstall(slug)) {
                        toast(upper(name) + " UNINSTALLED", theme::dim);
                        syncDevice();
                        rebuildView();
                      }
                    });
        break;
    }
  };
}

void App::openPage(PageTab t) {
  pages_->open(t);
  screen_ = Screen::Pages;
  screenSince_ = nowSeconds();
}

void App::openSystemMenu() {
  menu_ = Menu{};
  menu_.open = true;
  menu_.title = "VOLCANO OS";
  menu_.openedAt = nowSeconds();
  enum { Achievements = 1, Social, Profile, Refresh, Fullscreen, SignOut, Quit };
  bool in = screen_ == Screen::Home;
  if (in) {
    menu_.items.push_back({"ACHIEVEMENTS", "Everything you've unlocked", Achievements, !offline_ || opt_.demo});
    menu_.items.push_back({"SOCIAL", "Friends, requests and messages", Social, !offline_ || opt_.demo});
    menu_.items.push_back({"PROFILE", "Your stats, privacy and devices", Profile, !offline_ || opt_.demo});
    menu_.items.push_back({"REFRESH LIBRARY", "", Refresh, !offline_});
  }
  menu_.items.push_back({fullscreen_ ? "SWITCH TO WINDOWED" : "SWITCH TO FULLSCREEN", "F11", Fullscreen});
  if (in) menu_.items.push_back({"SIGN OUT", player_.gamertag.empty() ? "" : "Signed in as " + player_.gamertag, SignOut, true, true});
  menu_.items.push_back({"QUIT VOLCANO OS", "", Quit});
  menu_.sel = 0;
  menu_.onPick = [this](int id) {
    switch (id) {
      case Achievements: openPage(PageTab::Achievements); break;
      case Social: openPage(PageTab::Social); break;
      case Profile: openPage(PageTab::Profile); break;
      case Refresh: refreshLibrary(false); break;
      case Fullscreen: setFullscreen(!fullscreen_); break;
      case SignOut: signOut(); break;
      case Quit: running_ = false; break;
    }
  };
}

}  // namespace vos

// ---- in-game overlay ------------------------------------------------------------------

namespace vos {

void App::initOverlay() {
  overlayWin_ = createOverlayWindow();
  if (!overlayWin_) return;
  // Match the display's aspect so the overlay isn't stretched on ultrawides.
  SDL_Rect b{0, 0, 16, 9};
  SDL_GetDisplayBounds(std::max(0, SDL_GetWindowDisplayIndex(win_)), &b);
  int h = 720, w = std::max(960, std::min(2560, h * b.w / std::max(1, b.h)));
  ovSurf_ = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (ovSurf_) ovRen_ = SDL_CreateSoftwareRenderer(ovSurf_);
  if (ovRen_) {
    SDL_SetRenderDrawBlendMode(ovRen_, SDL_BLENDMODE_BLEND);
    ovReady_ = ovGfx_.init(nullptr, ovRen_);
  }
  if (!ovReady_) {
    logf("overlay window unavailable; drawing the overlay inside the launcher instead");
    shutdownOverlay();
  }
}

void App::shutdownOverlay() {
  if (ovReady_) ovGfx_.shutdown();
  ovReady_ = false;
  if (ovRen_) SDL_DestroyRenderer(ovRen_);
  if (ovSurf_) SDL_FreeSurface(ovSurf_);
  ovRen_ = nullptr;
  ovSurf_ = nullptr;
  overlayWin_.reset();
}

void App::beginSession() {
  gameStatus_.clear();
  panel_ = false;
  quitArmed_ = false;
  lastInvitePoll_ = nowSeconds() - 20;  // first invite check ~10s in
  gameToasts_.clear();
  double now = nowSeconds();
  gameToasts_.push_back({"info", "VOLCANO OS", "HOLD START + SELECT FOR THE MENU", now, now + 5});
}

void App::onGameEvent(const GameEvent& e) {
  if (sess_.kind == SessionKind::None) return;
  // Token bucket: a game can't flood the screen (burst 5, then 1 per second).
  double now = nowSeconds();
  evTokens_ = std::min(5.0, evTokens_ + (now - evLast_));
  evLast_ = now;
  if (e.kind == "status") {
    gameStatus_ = e.body;
    return;
  }
  if (evTokens_ < 1) return;
  evTokens_ -= 1;
  gameToasts_.push_back({e.kind, e.title.empty() ? upper(sess_.name) : e.title, e.body, now, now + 5.5});
  while (gameToasts_.size() > 3) gameToasts_.pop_front();
  if (e.kind == "achievement") input_.tick(0.35f, 60);
}

void App::openPanel() {
  panel_ = true;
  panelSel_ = 0;
  quitArmed_ = false;
  panelOpened_ = nowSeconds();
  panelFriends_.clear();
  if (!overlayWin_) SDL_RaiseWindow(win_);  // no native overlay: show it in the launcher window
  input_.tick(0.2f, 30);
  if (opt_.demo) {
    panelFriends_ = {{"Marlowe", "Playing Peggy!"}, {"ash_wednesday", "Online"}};
    return;
  }
  pool_->run<Result<std::string>>(
      main_, [this] { return api_.request("GET", "/api/friends"); },
      [this](Result<std::string> r) {
        if (!r.ok) return;
        json j = json::parse(r.value, nullptr, false);
        if (!j.is_array()) return;
        panelFriends_.clear();
        for (auto& f : j)
          if (f.is_object() && f.value("online", false)) panelFriends_.emplace_back(f.value("gamertag", ""), f.value("status", ""));
      });
}

void App::closePanel() {
  panel_ = false;
  quitArmed_ = false;
}

void App::panelAction() {
  bool native = sess_.kind == SessionKind::Native;
  if (panelSel_ == 0) {
    closePanel();
  } else if (panelSel_ == 1) {
    if (native) {
      closePanel();
      SDL_ShowWindow(win_);
      SDL_RaiseWindow(win_);
    } else {
      web_->close();
      closePanel();
    }
  } else if (panelSel_ == 2 && native) {
    if (!quitArmed_) {
      quitArmed_ = true;
      input_.tick(0.3f, 40);
    } else {
      proc_.terminate();
      closePanel();
    }
  }
}

void App::pollInvites(double now) {
  if (opt_.demo || sess_.slug.empty() || !api_.hasToken() || now - lastInvitePoll_ < 30) return;
  lastInvitePoll_ = now;
  pool_->run<Result<std::string>>(
      main_, [this, slug = sess_.slug] { return api_.request("GET", "/api/invites?game=" + urlEncode(slug)); },
      [this](Result<std::string> r) {
        if (!r.ok || sess_.kind == SessionKind::None) return;
        json j = json::parse(r.value, nullptr, false);
        if (!j.is_array()) return;
        for (auto& inv : j) {
          if (!inv.is_object() || !inv.contains("id") || !inv["id"].is_number()) continue;
          int64_t id = inv["id"].get<int64_t>();
          if (!seenInvites_.insert(id).second) continue;
          GameEvent e;
          e.kind = "toast";
          e.title = "GAME INVITE";
          e.body = upper(inv.value("from", "A friend")) + " INVITED YOU";
          onGameEvent(e);
        }
      });
}

void App::renderOverlay(double now) {
  while (!gameToasts_.empty() && gameToasts_.front().until < now) gameToasts_.pop_front();
  if (!overlayWin_ || !ovReady_) return;  // fallback path draws inside the launcher
  bool want = sess_.kind != SessionKind::None && (panel_ || !gameToasts_.empty());
  if (!want) {
    if (overlayWin_->visible()) overlayWin_->show(false);
    return;
  }
  ovGfx_.begin(Color(0, 0, 0, 0));
  drawGameToasts(ovGfx_, now);
  if (panel_) drawPanel(ovGfx_, now);
  SDL_RenderFlush(ovRen_);
  if (const char* dump = std::getenv("VOS_DUMP_OVERLAY")) IMG_SavePNG(ovSurf_, dump);  // debugging aid
  overlayWin_->present(ovSurf_->pixels, ovSurf_->w, ovSurf_->h, ovSurf_->pitch);
  overlayWin_->show(true);
}

}  // namespace vos
