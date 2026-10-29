// Launcher <-> game channel, so games can put things on the Volcano OS overlay.
//
// Native games get ZONTO_LAUNCHER_URL + ZONTO_LAUNCHER_TOKEN in their
// environment and POST JSON to a loopback-only HTTP server. Web games use
// window.ZontoLauncher (injected by the web renderer) which feeds the same
// event type. Everything is text-only and rate limited by the app.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "util.hpp"

namespace vos {

struct GameEvent {
  std::string kind;   // "toast" | "achievement" | "status"
  std::string title;  // <= 80 chars
  std::string body;   // <= 200 chars
};

class GameBridge {
 public:
  explicit GameBridge(MainQueue& main) : main_(main) {}
  ~GameBridge() { stop(); }

  // Binds 127.0.0.1:<random>; events are delivered on the main thread.
  bool start(std::function<void(GameEvent)> onEvent);
  void stop();
  int port() const { return port_; }
  const std::string& token() const { return token_; }

  // Shared by the HTTP path and the web renderer. Rejects malformed or
  // unknown event kinds; clamps lengths and strips control characters.
  static bool parse(const std::string& json, GameEvent& out);

 private:
  void serve();
  MainQueue& main_;
  std::function<void(GameEvent)> onEvent_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  int listenFd_ = -1;
  int port_ = 0;
  std::string token_;
};

}  // namespace vos
