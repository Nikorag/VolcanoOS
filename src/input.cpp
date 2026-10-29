#include "input.hpp"

#include <cmath>

#include "util.hpp"

namespace vos {

namespace {
constexpr float kStickOn = 0.55f;
constexpr float kStickOff = 0.35f;
constexpr double kRepeatFirst = 0.38;
constexpr double kRepeatRate = 0.105;
constexpr double kChordHold = 1.0;
}  // namespace

void Input::init() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // keep Home working while a game has focus
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);

  // Optional community mapping database next to the binary or in the data dir.
  for (const fs::path& p : {assetDir() / "gamecontrollerdb.txt", dataDir() / "gamecontrollerdb.txt"}) {
    std::error_code ec;
    if (fs::exists(p, ec)) SDL_GameControllerAddMappingsFromFile(p.string().c_str());
  }
  for (int i = 0; i < SDL_NumJoysticks(); ++i)
    if (SDL_IsGameController(i)) openPad(i);
}

void Input::shutdown() {
  for (auto& [id, pad] : pads_) SDL_GameControllerClose(pad);
  pads_.clear();
}

void Input::openPad(int index) {
  SDL_GameController* pad = SDL_GameControllerOpen(index);
  if (!pad) return;
  SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
  if (pads_.count(id)) {
    SDL_GameControllerClose(pad);
    return;
  }
  pads_[id] = pad;
  logf("controller connected: %s", SDL_GameControllerName(pad));
  refreshStyle();
}

void Input::closePad(SDL_JoystickID id) {
  auto it = pads_.find(id);
  if (it == pads_.end()) return;
  SDL_GameControllerClose(it->second);
  pads_.erase(it);
  for (auto& d : dir_) d.down = false;
  axisX_ = axisY_ = 0;
  refreshStyle();
}

void Input::refreshStyle() {
  if (pads_.empty()) {
    style_ = PadStyle::Keyboard;
    return;
  }
  switch (SDL_GameControllerGetType(pads_.begin()->second)) {
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
      style_ = PadStyle::PlayStation;
      break;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      style_ = PadStyle::Nintendo;
      break;
    default:
      style_ = PadStyle::Xbox;
  }
}

void Input::press(Action a, double now) {
  q_.push_back(a);
  lastActivity_ = now;
}

void Input::setDir(int idx, bool down, double now) {
  Held& h = dir_[idx];
  if (down && !h.down) {
    h.down = true;
    h.next = now + kRepeatFirst;
    press(static_cast<Action>(idx), now);
  } else if (!down) {
    h.down = false;
  }
}

bool Input::handle(const SDL_Event& e) {
  double now = nowSeconds();
  switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED:
      openPad(e.cdevice.which);
      return true;
    case SDL_CONTROLLERDEVICEREMOVED:
      closePad(e.cdevice.which);
      return true;

    case SDL_CONTROLLERBUTTONDOWN: {
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: setDir(0, true, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: setDir(1, true, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: setDir(2, true, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: setDir(3, true, now); break;
        case SDL_CONTROLLER_BUTTON_A: press(Action::Accept, now); break;
        case SDL_CONTROLLER_BUTTON_B: press(Action::Back, now); break;
        case SDL_CONTROLLER_BUTTON_X: press(Action::X, now); break;
        case SDL_CONTROLLER_BUTTON_Y: press(Action::Y, now); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: press(Action::LB, now); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: press(Action::RB, now); break;
        case SDL_CONTROLLER_BUTTON_GUIDE: press(Action::Home, now); break;
        case SDL_CONTROLLER_BUTTON_START:
          startDown_ = true;
          chordStart_ = now;
          chordFired_ = false;
          break;
        case SDL_CONTROLLER_BUTTON_BACK:
          selectDown_ = true;
          chordStart_ = now;
          chordFired_ = false;
          break;
        default: break;
      }
      if (style_ == PadStyle::Keyboard) refreshStyle();
      return true;
    }
    case SDL_CONTROLLERBUTTONUP: {
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: setDir(0, false, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: setDir(1, false, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: setDir(2, false, now); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: setDir(3, false, now); break;
        case SDL_CONTROLLER_BUTTON_START:
          // A tap (not the end of a chord) opens the menu.
          if (!chordFired_ && !selectDown_) press(Action::Start, now);
          startDown_ = false;
          break;
        case SDL_CONTROLLER_BUTTON_BACK:
          if (!chordFired_ && !startDown_) press(Action::Select, now);
          selectDown_ = false;
          break;
        default: break;
      }
      return true;
    }
    case SDL_CONTROLLERAXISMOTION: {
      float v = e.caxis.value / 32767.f;
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) axisX_ = v;
      else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) axisY_ = v;
      else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && v > 0.6f) press(Action::LB, now);
      else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && v > 0.6f) press(Action::RB, now);
      return true;
    }

    case SDL_KEYDOWN: {
      style_ = pads_.empty() ? PadStyle::Keyboard : style_;
      bool repeat = e.key.repeat != 0;
      switch (e.key.keysym.sym) {
        case SDLK_UP: case SDLK_w: press(Action::Up, now); return true;
        case SDLK_DOWN: case SDLK_s: press(Action::Down, now); return true;
        case SDLK_LEFT: case SDLK_a: press(Action::Left, now); return true;
        case SDLK_RIGHT: case SDLK_d: press(Action::Right, now); return true;
        default: break;
      }
      if (repeat) return true;
      switch (e.key.keysym.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: press(Action::Accept, now); return true;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: press(Action::Back, now); return true;
        case SDLK_x: press(Action::X, now); return true;
        case SDLK_y: case SDLK_r: case SDLK_F5: press(Action::Y, now); return true;
        case SDLK_q: case SDLK_PAGEUP: press(Action::LB, now); return true;
        case SDLK_e: case SDLK_PAGEDOWN: press(Action::RB, now); return true;
        case SDLK_TAB: case SDLK_m: press(Action::Start, now); return true;
        case SDLK_F10: case SDLK_HOME: press(Action::Home, now); return true;
        default: return false;
      }
    }
    default:
      return false;
  }
}

void Input::update(double now) {
  now_ = now;
  // Left stick -> directions, with hysteresis (on at 0.55, off at 0.35) so a
  // wobble around the threshold doesn't double-fire. A stick direction shares
  // the repeat slot with the dpad; the first to arrive owns the timer.
  struct Dir { int idx; bool on; bool off; };
  const Dir d[4] = {
      {0, axisY_ < -kStickOn, axisY_ > -kStickOff},
      {1, axisY_ > kStickOn, axisY_ < kStickOff},
      {2, axisX_ < -kStickOn, axisX_ > -kStickOff},
      {3, axisX_ > kStickOn, axisX_ < kStickOff},
  };
  for (const auto& x : d) {
    if (x.on && !stickDown_[x.idx]) {
      stickDown_[x.idx] = true;
      if (!dir_[x.idx].down) setDir(x.idx, true, now);
    } else if (x.off && stickDown_[x.idx]) {
      stickDown_[x.idx] = false;
      setDir(x.idx, false, now);
    }
  }
  // Auto-repeat for anything held.
  for (int i = 0; i < 4; ++i) {
    Held& h = dir_[i];
    if (h.down && now >= h.next) {
      h.next = now + kRepeatRate;
      press(static_cast<Action>(i), now);
    }
  }
  // Start + Select held together = Home, for pads whose Guide button the OS eats.
  if (startDown_ && selectDown_ && !chordFired_ && now - chordStart_ >= kChordHold) {
    chordFired_ = true;
    press(Action::Home, now);
  }
}

bool Input::poll(Action& out) {
  if (q_.empty()) return false;
  out = q_.front();
  q_.pop_front();
  return true;
}

void Input::tick(float strength, int ms) {
  for (auto& [id, pad] : pads_) {
    Uint16 s = static_cast<Uint16>(std::fmin(1.f, std::fmax(0.f, strength)) * 65535);
    SDL_GameControllerRumble(pad, s, s, static_cast<Uint32>(ms));
  }
}

}  // namespace vos
