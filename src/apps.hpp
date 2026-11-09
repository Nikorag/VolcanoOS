// Apps framework: self-contained "apps" (a media player, ...) that live beside games on Home.
//
// An app owns its own screens and its own account/state. The launcher gives it a context (drawing,
// artwork, a worker pool, toasts, the on-screen keyboard) and a way to hand the screen to an external
// player process. While that process runs the launcher keeps the controller and relays button presses
// back to the app, so the app can drive the player (seek, pause, subtitles, ...).
//
// Adding an app: derive from AppModule, implement the pure virtuals, register it in App::initApps().
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "art.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "runner.hpp"
#include "util.hpp"

namespace vos {

// What an app asks the launcher to run.
struct AppLaunch {
  std::string title;              // shown in the in-game menu
  fs::path exe;                   // absolute path to the player
  fs::path workdir;
  std::vector<std::string> args;  // joined and quoted by the launcher
  GameProcess::Env env;
};

struct AppContext {
  Gfx& gfx;
  Art& art;
  Pool& pool;
  MainQueue& main;
  bool demo = false;
  std::function<void(const std::string& text, Color color)> toast;
  // On-screen keyboard: title, initial text, max length, result (not called on cancel).
  std::function<void(const std::string& title, const std::string& initial, size_t maxLen,
                     std::function<void(const std::string&)> done)> text;
  // Hand the screen to a player process. Returns false (and toasts) if it could not start.
  std::function<bool(const AppLaunch&)> launch;
  std::function<void(const std::string& title, const std::string& msg, const std::string& okLabel,
                     std::function<void()> onOk)> confirm;
};

class AppModule {
 public:
  struct Info {
    std::string id;       // stable, lower-case: "jellyfin"
    std::string name;     // "Jellyfin"
    std::string tagline;  // one line under the name on Home
    std::string blurb;    // a few lines on Home
    std::string artUrl;   // file:// URL of the tile artwork ("" = generated placeholder)
    Color accent;
  };

  explicit AppModule(AppContext& c) : c_(c) {}
  virtual ~AppModule() = default;

  virtual Info info() const = 0;
  // Called every time the player opens the app from Home.
  virtual void open() = 0;
  // Returns true when the app wants to close (Back at its top level).
  virtual bool handle(Action a) = 0;
  virtual void draw(double now, float dt, PadStyle style) = 0;
  // Once per frame while the app is on screen or a playback session is running.
  virtual void tick(double now) {}
  // Fixture data for --demo and screenshots.
  virtual void loadDemo() {}
  // --demo --screen <app>-<variant>: jump to a named state for screenshots.
  virtual void demoScreen(const std::string& variant) {}
  // "Signed in as ..." style line for Home's status card; empty = none.
  virtual std::string status() const { return {}; }

  // ---- playback session (the app launched a player through AppContext::launch) ------------------
  // A press arrived while the player has the screen. Return true if it was used.
  virtual bool sessionAction(Action a) { return false; }
  virtual void sessionTick(double now) {}
  virtual void sessionEnded(int exitCode) {}
  // Short line for the in-game menu ("Episode 3 · 12:41 / 44:10").
  virtual std::string sessionStatus() const { return {}; }

 protected:
  AppContext& c_;
};

}  // namespace vos
