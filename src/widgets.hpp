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

float easeOut(float t);
float lerp(float a, float b, float t);

}  // namespace vos::ui
