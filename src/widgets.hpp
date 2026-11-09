// Reusable UI pieces: controller button prompts, chips, bars, placeholders.
#pragma once

#include <string>

#include "gfx.hpp"
#include "input.hpp"

namespace vos::ui {

// Button glyph for `a` in the style of the connected controller (or keyboard
// keycaps), followed by `label`. Returns the width consumed.
float prompt(Gfx& g, PadStyle style, Action a, const std::string& label, float x, float y, float size, float alpha = 1);
float promptWidth(Gfx& g, PadStyle style, Action a, const std::string& label, float size);

// Small bordered tag. Returns its width.
float chip(Gfx& g, const std::string& text, float x, float y, Color fg, Color border, float size = 15, Color fill = Color(0, 0, 0, 0));

void progressBar(Gfx& g, float x, float y, float w, float h, float frac, Color fill, Color track = theme::line);

// Primary action button (magma gradient when focused).
void button(Gfx& g, float x, float y, float w, float h, const std::string& label, Color accent, bool enabled, double t);

// Placeholder art: diagonal hatching over a tinted panel with the title's initials.
void placeholder(Gfx& g, float x, float y, float w, float h, const std::string& name, float alpha = 1);

// Deterministic accent per slug so tiles keep a stable identity.
Color tint(const std::string& slug);

// Notched panel corner ticks used to frame focused things.
void cornerTicks(Gfx& g, float x, float y, float w, float h, float len, float thick, Color c);

// Line icons (drawn from primitives) used across Control center and Settings.
enum class Icon { Trophy, Users, User, Refresh, Wifi, Bluetooth, Expand, SignOut, Power, Clock, Gamepad, Plus, Lock, Restart, Quit };
void icon(Gfx& g, Icon i, float cx, float cy, float size, Color c);

// Translucent warm-black card with a hairline border.
void glass(Gfx& g, float x, float y, float w, float h, float r, float alpha = 1.f, float fill = 0.74f);
// White focus ring hugging a rounded rect (drawn outside it).
void focusRing(Gfx& g, float x, float y, float w, float h, float r, float alpha = 1.f);
// Orange on/off switch, 68x38.
void toggleSwitch(Gfx& g, float x, float y, bool on, float alpha = 1.f);
// Spinner arc.
void spinner(Gfx& g, float cx, float cy, float r, double now);

float easeOut(float t);
float lerp(float a, float b, float t);

}  // namespace vos::ui
