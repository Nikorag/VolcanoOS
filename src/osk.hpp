// On-screen keyboard: d-pad moves, A types, X deletes, Y toggles shift,
// Start submits, B cancels. A physical keyboard types straight in.
#pragma once

#include <functional>
#include <string>

#include "gfx.hpp"
#include "input.hpp"

namespace vos {

class Osk {
 public:
  bool isOpen() const { return open_; }
  void show(const std::string& title, const std::string& initial, size_t maxLen, std::function<void(const std::string&)> done);
  bool handle(Action a);
  void typeText(const std::string& utf8);  // physical keyboard / SDL_TEXTINPUT
  void backspace();
  void submit();
  void cancel() { open_ = false; }
  void draw(Gfx& g, PadStyle style, double now);

 private:
  bool open_ = false;
  std::string title_, text_;
  size_t maxLen_ = 64;
  std::function<void(const std::string&)> done_;
  int row_ = 1, col_ = 0;
  bool shift_ = false;
  double openedAt_ = 0;
};

}  // namespace vos
