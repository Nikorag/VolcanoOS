#pragma once

#include <SDL.h>

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "api.hpp"
#include "art.hpp"
#include "auth.hpp"
#include "auth_device.hpp"
#include "bridge.hpp"
#include "background.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "installer.hpp"
#include "netctl.hpp"
#include "osk.hpp"
#include "qr.hpp"
#include "overlay.hpp"
#include "pages.hpp"
#include "runner.hpp"
#include "system_pages.hpp"
#include "util.hpp"
#include "webview.hpp"

namespace vos {

struct Options {
  bool demo = false;          // offline fixture library, fake installs
  bool windowed = false;
  bool fullscreen = false;    // force fullscreen even if settings say otherwise
  bool lint = false;          // --ui-lint: exit non-zero if any prompt overlaps text/buttons (see Gfx::lintBox)
  int width = 1600, height = 900;
  std::string shotPath;       // render N frames, write a PNG, exit
  int shotFrame = 150;
  std::string fakeGame;       // demo: really spawn this executable for "native" launches (overlay testing)
  std::string screen;         // demo: "signin" | "menu" | "system"
  std::vector<Action> script; // demo: synthetic inputs, one every few frames
};

class App {
 public:
  explicit App(Options opt);
  ~App();
  int run();

 private:
  // ---- types --------------------------------------------------------------
  enum class Screen { Boot, SignIn, Home, Pages, System };
  enum class Kind { Install, Update, Play, PlayWeb, Installing, Running, Unavailable };
  struct Primary {
    Kind kind;
    std::string label;
    bool enabled;
  };
  enum class Filter { All, Installed, Web };
  struct Toast {
    std::string text;
    Color color;
    double born, until;
  };
  struct MenuItem {
    std::string label, sub;
    int id;
    bool enabled = true;
    bool danger = false;
  };
  struct Menu {
    bool open = false;
    std::string title, message;
    std::vector<MenuItem> items;
    int sel = 0;
    double openedAt = 0;
    std::function<void(int)> onPick;
  };
  enum class SessionKind { None, Native, WebGame };
  struct Session {
    SessionKind kind = SessionKind::None;
    std::string slug, name;
    double start = 0, lastBeat = 0;
    bool selfReports = false;
  };

  // ---- lifecycle ------------------------------------------------------------
  bool initWindow();
  void loadSettings();
  void saveSettings();
  void boot();
  void frame(double now, float dt);
  void handleAction(Action a);

  // ---- account / library ------------------------------------------------------
  void startSignIn();         // phone sign-in (device code); falls back to the browser flow
  void startBrowserSignIn();
  void onSignedIn(const Credentials& creds);
  void resolveAccount();
  void signOut();
  void refreshLibrary(bool quiet);
  void applyLibrary(std::vector<GameEntry> g);
  void loadCachedLibrary();
  void rebuildView();
  void syncDevice();
  void loadDemo();

  // ---- game actions -------------------------------------------------------------
  Primary primaryFor(const GameEntry& g) const;
  void doPrimary(const GameEntry& g);
  void install(const GameEntry& g);
  void launchNative(const GameEntry& g);
  void launchWeb(const GameEntry& g);
  void endSession(bool crashed, int code);
  void pollSession(double now);
  const GameEntry* selected() const;

  // ---- ui -------------------------------------------------------------------------
  void drawBoot(double now);
  void drawSignIn(double now);
  void drawDeviceSignIn(double now);
  void drawHome(double now, float dt);
  void drawTopBar(double now);
  void drawPages(double now);
  void drawSystem(double now);
  void drawHero(const GameEntry* g, double now);
  void drawInfo(const GameEntry* g, double now);
  void drawShelf(double now, float dt);
  void drawTile(const GameEntry& g, float cx, float cy, float size, float focus, double now, bool selected);
  void drawHints(double now);
  void drawToasts(double now);
  void drawMenu(double now);
  void drawSession(double now);
  void drawMark(float cx, float cy, float size, double now) { drawMarkOn(gfx_, cx, cy, size, now); }
  void drawMarkOn(Gfx& g, float cx, float cy, float size, double now);
  void drawBadge(const GameEntry& g, float cx, float cy, float r, double now);

  void openGameMenu();
  void openSystemMenu();
  void openPage(PageTab t);
  void openSystem(SysTab t);
  void closeSystem();
  void checkFirstRunNetwork();
  void openConfirm(const std::string& title, const std::string& msg, const std::string& okLabel,
                   std::function<void()> onOk);
  void closeMenu() { menu_.open = false; }
  void toast(const std::string& text, Color c = theme::text, double secs = 4.0);
  void setFullscreen(bool on);
  void saveShot();

  // ---- in-game overlay ----------------------------------------------------------
  struct GameToast {
    std::string kind, title, body;
    double born, until;
  };
  void initOverlay();
  void shutdownOverlay();
  void beginSession();
  void onGameEvent(const GameEvent& e);
  void openPanel();
  void closePanel();
  void panelAction();
  void pollInvites(double now);
  void renderOverlay(double now);
  void drawGameToasts(Gfx& g, double now);
  void drawPanel(Gfx& g, double now);

  // ---- state -------------------------------------------------------------------------
  Options opt_;
  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  Gfx gfx_;
  Input input_;
  MainQueue main_;
  std::unique_ptr<NetCtl> netctl_;  // declared before pool_ so queued jobs never outlive it
  std::unique_ptr<Pool> pool_;
  Api api_;
  std::unique_ptr<Installer> installer_;
  std::unique_ptr<Art> art_;
  std::unique_ptr<Pages> pages_;
  std::unique_ptr<SystemPages> sys_;
  Screen sysReturn_ = Screen::Home;  // where Back from the system screen goes
  Osk osk_;
  Background bg_;
  SignInFlow signin_;
  DeviceSignIn device_;
  SDL_Texture* qrTex_ = nullptr;  // QR of the current verification URL, 1 px per module
  int qrSize_ = 0;
  std::string qrFor_;
  std::unique_ptr<WebView> web_;
  GameProcess proc_;

  bool running_ = true;
  bool fullscreen_ = true;
  Screen screen_ = Screen::Boot;
  double screenSince_ = 0;
  std::string bootStatus_ = "STARTING";
  std::string signInError_;
  bool signInBusy_ = false;

  Player player_;
  bool offline_ = false;
  bool loadingLibrary_ = false;
  std::vector<GameEntry> games_;
  std::vector<int> view_;
  Filter filter_ = Filter::All;
  int sel_ = 0;
  float cam_ = 0;
  std::unordered_map<std::string, float> focusAnim_;
  struct HitRect { float x, y, w, h; int idx; };
  std::vector<HitRect> hits_;

  std::string heroCur_, heroPrev_;
  double heroSince_ = 0;
  float infoSlide_ = 1;
  std::string infoSlug_;

  // Overlay state. When the platform has no native overlay window the same
  // content is drawn inside the launcher window instead.
  std::unique_ptr<GameBridge> bridge_;
  std::unique_ptr<OverlayWindow> overlayWin_;
  SDL_Surface* ovSurf_ = nullptr;
  SDL_Renderer* ovRen_ = nullptr;
  Gfx ovGfx_;
  bool ovReady_ = false;
  std::deque<GameToast> gameToasts_;
  bool panel_ = false, quitArmed_ = false;
  int panelSel_ = 0;
  double panelOpened_ = 0;
  std::string gameStatus_;
  std::vector<std::pair<std::string, std::string>> panelFriends_;
  double lastInvitePoll_ = 0, evTokens_ = 5, evLast_ = 0;
  std::set<int64_t> seenInvites_;

  Session sess_;
  bool launching_ = false;
  Menu menu_;
  std::deque<Toast> toasts_;

  uint64_t frameNo_ = 0;
  double startTime_ = 0;
  size_t scriptPos_ = 0;
  bool shotTaken_ = false;
  double lastMouseMove_ = 0;
};

}  // namespace vos
