// Controller-first input. Every device collapses to the same small set of
// actions so screens never care what's plugged in. Gamepads are primary; the
// keyboard maps onto the same actions so the launcher is usable without one.
#pragma once

#include <SDL.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace vos {

enum class Action {
  Up, Down, Left, Right,
  Accept,   // A / Cross / Enter
  Back,     // B / Circle / Esc
  X,        // X / Square  - options
  Y,        // Y / Triangle - refresh
  LB, RB,   // shoulders   - switch filter
  Start,    // menu
  Select,   // back/view
  Home,     // Guide, or Start+Select held: leave the current game / open the system menu
};

enum class PadStyle { Keyboard, Xbox, PlayStation, Nintendo };

class Input {
 public:
  void init();
  void shutdown();

  // Feed every SDL event through here. Returns true if it was consumed.
  bool handle(const SDL_Event& e);
  // Per-frame: stick/dpad repeat and chord timing.
  void update(double now);
  bool poll(Action& out);
  void push(Action a) { q_.push_back(a); }

  // Linux: while a game owns the keyboard focus the launcher gets no key events, so F10 is read
  // straight from the keyboard devices (/dev/input) and becomes Action::Home. Only armed on request.
  void setBackgroundHotkey(bool armed) { bgArmed_ = armed; }

  PadStyle style() const { return style_; }
  bool hasPad() const { return !pads_.empty(); }
  int padCount() const { return static_cast<int>(pads_.size()); }
  // Small haptic tick on the active pad (no-op without rumble support).
  void tick(float strength = 0.25f, int ms = 30);
  double lastActivity() const { return lastActivity_; }

 private:
  struct Held {
    bool down = false;
    double next = 0;
  };
  void press(Action a, double now);
  void setDir(int idx, bool down, double now);
  void openPad(int deviceIndex);
  void closePad(SDL_JoystickID id);
  void refreshStyle();

  std::map<SDL_JoystickID, SDL_GameController*> pads_;
  std::deque<Action> q_;
  Held dir_[4];  // up, down, left, right
  float axisX_ = 0, axisY_ = 0;
  bool stickDown_[4] = {false, false, false, false};
  bool startDown_ = false, selectDown_ = false;
  double chordStart_ = 0;
  bool chordFired_ = false;
  void startKeyWatcher();
  void stopKeyWatcher();
  void keyWatcherMain();
  std::thread keyThread_;
  std::atomic<bool> keyStop_{false};
  std::atomic<bool> bgArmed_{false};
  std::atomic<int> bgHome_{0};
  PadStyle style_ = PadStyle::Keyboard;
  double lastActivity_ = 0;
  double now_ = 0;
};

}  // namespace vos
