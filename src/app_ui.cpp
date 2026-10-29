// Everything that draws: boot, sign-in, the Home shelf, menus, toasts.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app.hpp"
#include "widgets.hpp"

namespace vos {

namespace {

constexpr float kMargin = 96.f;

TextStyle mono(float size, Color c, float tracking = 0) {
  TextStyle t;
  t.font = Font::Mono;
  t.size = size;
  t.color = c;
  t.tracking = tracking;
  return t;
}
TextStyle head(float size, Color c, float tracking = 0, bool bold = true) {
  TextStyle t;
  t.font = Font::Head;
  t.size = size;
  t.color = c;
  t.tracking = tracking;
  t.bold = bold;
  return t;
}

std::string clockText() {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M", &tmv);
  return buf;
}

std::string elapsedText(double secs) {
  int s = static_cast<int>(secs);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return buf;
}

// Truncate to roughly `maxChars` on a word boundary.
std::string clampText(const std::string& s, size_t maxChars) {
  if (s.size() <= maxChars) return s;
  size_t cut = s.rfind(' ', maxChars);
  if (cut == std::string::npos || cut < maxChars / 2) cut = maxChars;
  return s.substr(0, cut) + "\xE2\x80\xA6";
}

void shadowText(Gfx& g, const std::string& s, float x, float y, TextStyle st, float off = 3) {
  TextStyle sh = st;
  sh.color = Color(0, 0, 0, static_cast<uint8_t>(st.color.a * 0.65f));
  g.text(s, x + off, y + off, sh);
  g.text(s, x, y, st);
}

void arrow(Gfx& g, float cx, float cy, float r, bool up, Color c) {
  float d = up ? -1.f : 1.f;
  g.line(cx, cy - r * d, cx, cy + r * d, 2.4f, c);
  g.line(cx - r * 0.75f, cy + r * 0.25f * d, cx, cy + r * d, 2.4f, c);
  g.line(cx + r * 0.75f, cy + r * 0.25f * d, cx, cy + r * d, 2.4f, c);
}

}  // namespace

// ---- logo ---------------------------------------------------------------------

void App::drawMarkOn(Gfx& g, float cx, float cy, float s, double now) {
  float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(now) * 2.1f);
  g.glow(cx, cy - s * 0.2f, s * 1.1f, s * 0.8f, Color(255, 100, 30, static_cast<uint8_t>(70 * pulse)));
  std::vector<SDL_FPoint> cone = {{cx - s * 0.62f, cy + s * 0.42f}, {cx - s * 0.15f, cy - s * 0.30f}, {cx, cy - s * 0.17f},
                                  {cx + s * 0.15f, cy - s * 0.30f}, {cx + s * 0.62f, cy + s * 0.42f}};
  g.polygon(cone, Color(46, 30, 24));
  for (size_t i = 0; i + 1 < cone.size(); ++i)
    g.line(cone[i].x, cone[i].y, cone[i + 1].x, cone[i + 1].y, std::max(1.6f, s * 0.07f), theme::magma);
  g.glow(cx, cy - s * 0.24f, s * 0.35f, s * 0.2f, Color(255, 210, 120, static_cast<uint8_t>(210 * pulse)));
  // ember
  float ey = cy - s * 0.55f - std::fmod(static_cast<float>(now) * 14, s * 0.5f);
  g.glow(cx + std::sin(static_cast<float>(now) * 3) * s * 0.08f, ey, s * 0.09f, s * 0.09f, theme::ember);
}

// ---- frame dispatch ---------------------------------------------------------------

void App::frame(double now, float dt) {
  gfx_.begin();

  if (screen_ == Screen::SignIn) {
    auto st = signin_.state();
    if (st == SignInFlow::State::Done) {
      std::string tok = signin_.token();
      signin_.cancel();
      onSignedIn(tok);
    } else if (st == SignInFlow::State::Failed) {
      signInError_ = upper(signin_.error());
      signin_.cancel();
    }
  }

  if (sess_.kind != SessionKind::None) {
    drawSession(now);
  } else {
    switch (screen_) {
      case Screen::Boot: drawBoot(now); break;
      case Screen::SignIn: drawSignIn(now); break;
      case Screen::Home: drawHome(now, dt); break;
      case Screen::Pages: drawPages(now); break;
    }
  }
  renderOverlay(now);
  drawMenu(now);
  osk_.draw(gfx_, input_.style(), now);
  drawToasts(now);

  // Screen fade-in.
  float f = ui::easeOut(static_cast<float>((now - screenSince_) / 0.5));
  if (f < 1) gfx_.rect(0, 0, gfx_.W(), gfx_.H(), Color(0, 0, 0, static_cast<uint8_t>((1 - f) * 255)));
  gfx_.present();
}

// ---- boot / sign-in ---------------------------------------------------------------

static void wordmark(Gfx& g, float cx, float y, float size, double now, bool tag) {
  TextStyle a = head(size, theme::text, size * 0.16f);
  a.align = Align::Center;
  Vec2 m = g.measure("VOLCANO OS", a);
  // Two-tone: VOLCANO in white, OS in magma.
  TextStyle v = a;
  v.align = Align::Left;
  float vw = g.measure("VOLCANO ", v).x;
  float x0 = cx - m.x / 2;
  g.text("VOLCANO ", x0, y, v);
  TextStyle o = v;
  o.color = theme::magma;
  g.text("OS", x0 + vw, y, o);
  if (tag) {
    TextStyle t = mono(size * 0.17f, theme::dim, size * 0.05f);
    t.align = Align::Center;
    g.text("THE ZONTO GAMES BIG PICTURE LAUNCHER", cx, y + m.y + size * 0.12f, t);
  }
  (void)now;
}

void App::drawBoot(double now) {
  bg_.draw(gfx_, now, 0.15f);
  float W = gfx_.W(), H = gfx_.H();
  drawMark(W / 2, H * 0.36f, 96, now);
  wordmark(gfx_, W / 2, H * 0.47f, 78, now, true);
  TextStyle t = mono(17, theme::dim, 5);
  t.align = Align::Center;
  gfx_.text(bootStatus_, W / 2, H * 0.72f, t);
  for (int i = 0; i < 3; ++i) {
    float k = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 4 - i * 0.8f);
    gfx_.circle(W / 2 - 24 + i * 24, H * 0.72f + 48, 4, theme::magma.alpha(0.25f + 0.75f * k));
  }
}

void App::drawSignIn(double now) {
  bg_.draw(gfx_, now, 0.05f);
  float W = gfx_.W(), H = gfx_.H();
  drawMark(W / 2, H * 0.26f, 90, now);
  wordmark(gfx_, W / 2, H * 0.36f, 92, now, true);

  auto st = signin_.state();
  float py = H * 0.62f;
  float pw = 760, ph = 230, px = W / 2 - pw / 2;
  gfx_.rrect(px, py, pw, ph, 10, Color(14, 11, 10, 220));
  gfx_.rrectLine(px, py, pw, ph, 10, 1.5f, theme::line);
  ui::cornerTicks(gfx_, px - 6, py - 6, pw + 12, ph + 12, 22, 2, theme::magma.alpha(0.7f));

  TextStyle title = head(26, theme::text, 5);
  title.align = Align::Center;
  TextStyle sub = mono(16, theme::dim, 2);
  sub.align = Align::Center;

  if (st == SignInFlow::State::Waiting || st == SignInFlow::State::Exchanging) {
    gfx_.arc(W / 2, py + 66, 22, 4, static_cast<float>(now) * 4, static_cast<float>(now) * 4 + 4.2f, theme::magma);
    gfx_.text(st == SignInFlow::State::Waiting ? "WAITING FOR YOUR BROWSER" : "FINISHING UP", W / 2, py + 108, title);
    std::string url = signin_.authorizeUrl();
    sub.size = 14;
    gfx_.text(st == SignInFlow::State::Waiting ? "Sign in on the page that just opened, then come back." : "Almost there.", W / 2, py + 148, sub);
    if (!url.empty() && st == SignInFlow::State::Waiting) {
      TextStyle u = mono(12, theme::faint, 0.5f);
      u.align = Align::Center;
      gfx_.text(gfx_.ellipsize(url, u, pw - 60), W / 2, py + 176, u);
    }
    if (st == SignInFlow::State::Waiting)
      ui::prompt(gfx_, input_.style(), Action::Back, "CANCEL", W / 2 - 62, py + ph - 52, 22);
  } else if (signInBusy_) {
    gfx_.arc(W / 2, py + 100, 22, 4, static_cast<float>(now) * 4, static_cast<float>(now) * 4 + 4.2f, theme::magma);
    gfx_.text("CONNECTING", W / 2, py + 144, title);
  } else {
    gfx_.text(api_.hasToken() ? "RECONNECT" : "SIGN IN WITH ZONTO", W / 2, py + 36, title);
    if (!signInError_.empty()) {
      sub.color = theme::err;
      gfx_.text(signInError_, W / 2, py + 84, sub);
    } else {
      gfx_.text("Your browser handles the login, so Google sign-in just works.", W / 2, py + 84, sub);
    }
    ui::button(gfx_, W / 2 - 190, py + 124, 380, 66, api_.hasToken() ? "TRY AGAIN" : "SIGN IN", theme::magma, true, now);
    ui::prompt(gfx_, input_.style(), Action::Accept, "", W / 2 - 170, py + 124 + 33 - 12, 22);
  }

  // Footer.
  gfx_.rect(kMargin, H - 74, W - kMargin * 2, 1, theme::line.alpha(0.6f));
  TextStyle f = mono(14, theme::faint, 3);
  gfx_.text("WWW.NIKORAG.CO.UK", kMargin, H - 52, f);
  float x = W - kMargin;
  float w = ui::promptWidth(gfx_, input_.style(), Action::Start, "MENU", 22);
  ui::prompt(gfx_, input_.style(), Action::Start, "MENU", x - w, H - 56, 22);
}

// ---- home ----------------------------------------------------------------------------

void App::drawHome(double now, float dt) {
  bg_.draw(gfx_, now, 0.30f);
  const GameEntry* g = selected();

  // Track hero changes for the cross-fade and the info slide.
  std::string url = g ? g->heroUrl : "";
  if (url != heroCur_) {
    heroPrev_ = heroCur_;
    heroCur_ = url;
    heroSince_ = now;
  }
  std::string slug = g ? g->slug : "";
  if (slug != infoSlug_) {
    infoSlug_ = slug;
    infoSlide_ = 0;
  }
  infoSlide_ = std::min(1.f, infoSlide_ + dt * 4.5f);

  drawHero(g, now);
  // Left scrim so text always reads, whatever the art.
  gfx_.hgrad(0, 0, gfx_.W() * 0.62f, gfx_.H(), Color(8, 6, 6, 215), Color(8, 6, 6, 0));
  gfx_.vgrad(0, gfx_.H() - 360, gfx_.W(), 360, Color(8, 6, 6, 0), Color(8, 6, 6, 235));

  drawTopBar(now);
  if (g) drawInfo(g, now);
  else {
    TextStyle t = head(54, theme::dim, 4);
    gfx_.text(filter_ == Filter::All ? "NO GAMES YET" : "NOTHING HERE", kMargin, 300, t);
    TextStyle s = mono(18, theme::faint, 2);
    gfx_.text(filter_ == Filter::All ? "Open the menu and visit the store to add some." : "Try another filter with LB / RB.", kMargin, 380, s);
  }
  drawShelf(now, dt);
  drawHints(now);
}

void App::drawTopBar(double now) {
  const float W = gfx_.W(), cy = 62;
  drawMark(kMargin + 22, cy + 4, 34, now);
  TextStyle w = head(26, theme::text, 7);
  float vw = gfx_.text("VOLCANO ", kMargin + 62, cy - 15, w).x;
  w.color = theme::magma;
  gfx_.text("OS", kMargin + 62 + vw, cy - 15, w);

  // Tabs with shoulder glyphs: library filters on Home, pages elsewhere.
  const bool onPages = screen_ == Screen::Pages;
  const char* labels[3] = {"ALL", "INSTALLED", "WEB"};
  if (onPages)
    for (int i = 0; i < 3; ++i) labels[i] = Pages::label(i);
  const int activeTab = onPages ? static_cast<int>(pages_->tab()) : static_cast<int>(filter_);
  float x = 520;
  float gw = ui::promptWidth(gfx_, input_.style(), Action::LB, "", 20);
  ui::prompt(gfx_, input_.style(), Action::LB, "", x, cy - 12, 20);
  x += gw + 26;
  for (int i = 0; i < 3; ++i) {
    bool on = activeTab == i;
    TextStyle t = mono(18, on ? theme::text : theme::faint, 4);
    t.bold = on;
    Vec2 m = gfx_.measure(labels[i], t);
    gfx_.text(labels[i], x, cy - m.y / 2, t);
    if (on) {
      gfx_.rect(x, cy + 18, m.x, 3, theme::magma);
      gfx_.glow(x + m.x / 2, cy + 19, m.x * 0.8f, 14, theme::magma.alpha(0.35f));
    }
    x += m.x + 40;
  }
  ui::prompt(gfx_, input_.style(), Action::RB, "", x - 14, cy - 12, 20);

  // Right cluster: player, clock, device.
  float rx = W - kMargin;
  if (!player_.gamertag.empty()) {
    TextStyle n = head(20, theme::text, 2);
    Vec2 m = gfx_.measure(upper(player_.gamertag), n);
    rx -= 46;
    gfx_.circle(rx + 23, cy, 23, theme::panelHi);
    gfx_.ring(rx + 23, cy, 23, 2, theme::magma);
    TextStyle in = head(22, theme::magma, 0);
    in.align = Align::Center;
    std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(player_.gamertag[0]))));
    gfx_.text(ini, rx + 23, cy - gfx_.measure(ini, in).y / 2, in);
    rx -= 16 + m.x;
    gfx_.text(upper(player_.gamertag), rx, cy - 20, n);
    TextStyle lv = mono(13, theme::dim, 3);
    gfx_.text("LEVEL " + std::to_string(player_.level), rx, cy + 4, lv);
    rx -= 36;
  }
  TextStyle c = mono(22, theme::dim, 2);
  std::string clock = clockText();
  Vec2 cm = gfx_.measure(clock, c);
  rx -= cm.x;
  gfx_.text(clock, rx, cy - cm.y / 2, c);
  rx -= 34;
  if (offline_) {
    TextStyle oc = mono(13, theme::warn, 2.2f);
    float cw = gfx_.measure("OFFLINE", oc).x + 13 * 1.3f;
    ui::chip(gfx_, "OFFLINE", rx - cw, cy - 13, theme::warn, theme::warn, 13);
    rx -= cw + 20;
  }
  // Controller indicator.
  bool pad = input_.hasPad();
  gfx_.circle(rx - 6, cy, 5, pad ? theme::ok : theme::faint);
  if (pad) gfx_.glow(rx - 6, cy, 14, 14, theme::ok.alpha(0.35f));
}

void App::drawPages(double now) {
  bg_.draw(gfx_, now, 0.62f);
  drawTopBar(now);
  pages_->draw(now, 0.016f, input_.style());
}

void App::drawHero(const GameEntry* g, double now) {
  const float W = gfx_.W();
  const float hx = W * 0.30f, hw = W - hx, hh = 800;
  float k = ui::easeOut(static_cast<float>((now - heroSince_) / 0.6));

  auto one = [&](const std::string& u, float a) {
    if (a <= 0 || u.empty()) return;
    if (const ArtImage* img = art_->get(u))
      gfx_.imageFeather(img->tex, img->w, img->h, hx, 0, hw, hh, a * art_->fade(img, now), 0.55f, 0, 0.10f, 0.50f);
  };
  one(heroPrev_, 1 - k);
  one(heroCur_, k);

  // No hero art: show the logo large and soft instead of an empty hole.
  if (g && g->heroUrl.empty()) {
    if (const ArtImage* logo = art_->get(g->logoUrl)) {
      float s = 420;
      gfx_.glow(W * 0.74f, 330, 380, 300, ui::tint(g->slug).alpha(0.22f));
      gfx_.contain(logo->tex, logo->w, logo->h, W * 0.74f - s / 2, 330 - s / 2, s, s, 0.9f * art_->fade(logo, now));
    } else {
      // Big hatched emblem keyed to the title.
      Color t = ui::tint(g->slug);
      gfx_.glow(W * 0.74f, 330, 420, 320, t.alpha(0.20f));
    }
  }
}

void App::drawInfo(const GameEntry* g, double now) {
  const float x0 = kMargin;
  float slide = ui::easeOut(infoSlide_);
  float ox = (1 - slide) * -36, a = slide;
  Primary prim = primaryFor(*g);

  // Tags.
  std::string tags = upper(g->tags.empty() ? (g->isWeb() ? "WEB GAME" : "GAME") : g->tags);
  gfx_.text(tags, x0 + ox, 176, mono(17, theme::magma.alpha(a), 5));

  // Title, shrunk to fit one line.
  std::string title = upper(g->name);
  float maxW = gfx_.W() * 0.5f;
  float size = 98;
  TextStyle ts = head(size, theme::text.alpha(a), 1.5f);
  while (size > 52 && gfx_.measure(title, ts).x > maxW) {
    size -= 6;
    ts.size = size;
  }
  title = gfx_.ellipsize(title, ts, maxW);
  Vec2 tm = gfx_.measure(title, ts);
  shadowText(gfx_, title, x0 + ox, 208, ts, 3);

  // Blurb, capped at three lines.
  TextStyle bs = head(24, theme::dim.alpha(a), 0, false);
  bs.wrap = std::min(820.f, gfx_.W() * 0.46f);
  bs.lineHeight = 1.18f;
  std::string blurb = g->blurb.empty() ? "No description yet." : clampText(g->blurb, 190);
  gfx_.text(blurb, x0 + ox, 208 + tm.y + 18, bs);

  // Stat row.
  struct Stat { std::string label, value; Color c; float bar; };
  std::vector<Stat> stats;
  stats.push_back({"PLAYTIME", humanPlaytime(g->minutesPlayed), theme::text, -1});
  stats.push_back({"ACHIEVEMENTS", g->achTotal ? std::to_string(g->achUnlocked) + " / " + std::to_string(g->achTotal) : "NONE",
                   theme::text, g->achTotal ? static_cast<float>(g->achUnlocked) / g->achTotal : -1});
  const Installed* inst = installer_->installed(g->slug);
  std::string status;
  Color sc = theme::text;
  if (inst && g->hasBuild && inst->version != g->build.version) { status = "UPDATE READY"; sc = theme::ember; }
  else if (inst) { status = "INSTALLED  V" + inst->version; sc = theme::ok; }
  else if (g->hasBuild) { status = "NOT INSTALLED"; sc = theme::dim; }
  else if (g->isWeb()) { status = "PLAYS IN BROWSER"; sc = theme::text; }
  else { status = "UNAVAILABLE"; sc = theme::faint; }
  stats.push_back({"STATUS", status, sc, -1});
  if (g->hasBuild && prim.kind != Kind::Play)
    stats.push_back({inst ? "UPDATE SIZE" : "DOWNLOAD", humanBytes(g->build.sizeBytes), theme::text, -1});

  float sx = x0 + ox, sy = 520;
  for (auto& s : stats) {
    TextStyle l = mono(13, theme::faint.alpha(a), 3.5f);
    gfx_.text(s.label, sx, sy, l);
    TextStyle v = head(30, s.c.alpha(a), 1);
    Vec2 vm = gfx_.measure(s.value, v);
    gfx_.text(s.value, sx, sy + 24, v);
    float colW = std::max(vm.x, 150.f);
    if (s.bar >= 0) ui::progressBar(gfx_, sx, sy + 66, colW, 5, s.bar, theme::ember);
    sx += colW + 56;
  }

  // Action area.
  float by = prim.kind == Kind::Installing ? 586.f : 616.f;
  if (prim.kind == Kind::Installing) {
    auto j = installer_->job(g->slug);
    float frac = j ? j->fraction() : 0;
    std::string phase = j ? j->phaseLabel() : "WORKING";
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(frac * 100));
    TextStyle l = head(26, theme::ember, 4);
    gfx_.text(phase, x0 + ox, by, l);
    TextStyle r = mono(20, theme::text, 2);
    std::string detail = std::string(pct);
    if (j && j->phase.load() == InstallJob::Downloading)
      detail += "   " + humanBytes(j->done.load()) + " / " + humanBytes(j->total.load());
    gfx_.text(detail, x0 + ox, by + 40, r);
    ui::progressBar(gfx_, x0 + ox, by + 78, 620, 10, frac, theme::magma);
    gfx_.glow(x0 + ox + 620 * frac, by + 83, 40, 22, theme::magma.alpha(0.4f));
    ui::prompt(gfx_, input_.style(), Action::Back, "CANCEL", x0 + ox, by + 106, 22);
  } else {
    Color accent = prim.kind == Kind::Update ? theme::ember : prim.kind == Kind::Install ? theme::magma : theme::magma;
    if (prim.kind == Kind::Play) accent = Color(255, 122, 40);
    float bw = std::max(400.f, 150 + gfx_.measure(prim.label, head(30, theme::text, 4)).x);
    ui::button(gfx_, x0 + ox, by, bw, 84, prim.label, accent, prim.enabled, now);
    if (prim.enabled) ui::prompt(gfx_, input_.style(), Action::Accept, "", x0 + ox + 26, by + 84 / 2.f - 14, 24);
    float px = x0 + ox + bw + 40;
    px += ui::prompt(gfx_, input_.style(), Action::X, "OPTIONS", px, by + 28, 24) + 34;
  }
}

void App::drawBadge(const GameEntry& g, float cx, float cy, float r, double now) {
  Primary p = primaryFor(g);
  if (p.kind == Kind::PlayWeb || p.kind == Kind::Unavailable) return;
  gfx_.circle(cx, cy, r + 3, Color(8, 6, 6, 190));
  switch (p.kind) {
    case Kind::Running: {
      float k = 0.6f + 0.4f * std::sin(static_cast<float>(now) * 5);
      gfx_.circle(cx, cy, r * 0.55f, theme::ok.alpha(k));
      gfx_.glow(cx, cy, r * 2, r * 2, theme::ok.alpha(0.3f * k));
      break;
    }
    case Kind::Installing: {
      auto j = installer_->job(g.slug);
      float f = j ? j->fraction() : 0;
      gfx_.ring(cx, cy, r, 3, theme::line);
      gfx_.arc(cx, cy, r, 3, -1.5708f, -1.5708f + 6.2831f * std::max(0.03f, f), theme::magma);
      break;
    }
    case Kind::Update:
      gfx_.ring(cx, cy, r, 2.4f, theme::ember);
      arrow(gfx_, cx, cy, r * 0.5f, true, theme::ember);
      break;
    case Kind::Install:
      gfx_.ring(cx, cy, r, 2.4f, theme::dim);
      arrow(gfx_, cx, cy, r * 0.5f, false, theme::dim);
      break;
    case Kind::Play:
      gfx_.ring(cx, cy, r, 2.4f, theme::ok);
      gfx_.line(cx - r * 0.42f, cy + 1, cx - r * 0.1f, cy + r * 0.36f, 2.6f, theme::ok);
      gfx_.line(cx - r * 0.1f, cy + r * 0.36f, cx + r * 0.46f, cy - r * 0.34f, 2.6f, theme::ok);
      break;
    default: break;
  }
}

void App::drawTile(const GameEntry& g, float cx, float cy, float size, float focus, double now, bool selected) {
  float s = size * (1 + 0.14f * focus);
  float x = cx - s / 2, y = cy - s / 2 - 8 * focus;
  const float r = 12;

  if (focus > 0.02f) {
    gfx_.glow(cx, y + s / 2, s * 0.95f, s * 0.85f, theme::magma.alpha(0.30f * focus));
  }
  // Base + art.
  gfx_.rrect(x, y + 6 * focus, s, s, r, Color(0, 0, 0, static_cast<uint8_t>(90 * focus)));
  const ArtImage* logo = art_->get(g.logoUrl);
  if (logo) {
    gfx_.rrectGrad(x, y, s, s, r, Color(40, 31, 27), Color(22, 17, 15));
    float pad = s * 0.12f;
    gfx_.contain(logo->tex, logo->w, logo->h, x + pad, y + pad, s - pad * 2, s - pad * 2 - 18, art_->fade(logo, now));
  } else {
    ui::placeholder(gfx_, x, y, s, s, g.slug);
  }
  // Name strip.
  gfx_.vgrad(x + 1, y + s * 0.62f, s - 2, s * 0.38f - 1, Color(8, 6, 6, 0), Color(8, 6, 6, 235));
  TextStyle n = mono(14 + 2 * focus, theme::text.alpha(0.55f + 0.45f * focus), 1.8f);
  gfx_.text(gfx_.ellipsize(upper(g.name), n, s - 28), x + 14, y + s - 28, n);

  // Frame.
  if (selected) {
    gfx_.rrectLine(x - 3, y - 3, s + 6, s + 6, r + 3, 3, theme::magma);
    float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 4);
    gfx_.rrectLine(x - 8, y - 8, s + 16, s + 16, r + 8, 1.5f, theme::ember.alpha(0.25f + 0.25f * pulse));
  } else {
    gfx_.rrectLine(x, y, s, s, r, 1.2f, theme::line.alpha(0.9f));
  }
  if (g.isWeb()) {
    TextStyle w = mono(11, Color(15, 10, 8), 2);
    float cw = gfx_.measure("WEB", w).x + 14;
    gfx_.rrect(x + 10, y + 10, cw, 20, 3, theme::ember);
    gfx_.text("WEB", x + 17, y + 12, w);
  }
  drawBadge(g, x + s - 26, y + 27, 13, now);
}

void App::drawShelf(double now, float dt) {
  const float W = gfx_.W();
  const float T = 196, G = 30, cy = 890;
  const float step = T + G;
  int n = static_cast<int>(view_.size());

  // Section label.
  TextStyle l = mono(14, theme::faint, 5);
  gfx_.text("LIBRARY", kMargin, 724, l);
  std::string count = std::to_string(n) + (n == 1 ? " TITLE" : " TITLES");
  gfx_.text(count, kMargin + 120, 724, mono(14, theme::dim, 3));
  if (loadingLibrary_) {
    gfx_.arc(kMargin + 270, 730, 8, 2.5f, static_cast<float>(now) * 6, static_cast<float>(now) * 6 + 4, theme::magma);
    gfx_.text("SYNCING", kMargin + 288, 724, mono(14, theme::magma, 3));
  }
  gfx_.rect(kMargin, 750, W - kMargin * 2, 1, theme::line.alpha(0.55f));

  hits_.clear();
  if (n == 0) return;

  // Keep the selection in the third slot; ease the camera toward it.
  float visible = W - kMargin * 2;
  float maxCam = std::max(0.f, n * step - G - visible);
  float target = std::min(maxCam, std::max(0.f, sel_ * step - 2 * step));
  cam_ += (target - cam_) * (1 - std::exp(-dt * 11));

  auto cxOf = [&](int i) { return kMargin + T / 2 + i * step - cam_; };
  auto focusOf = [&](int i) {
    float& f = focusAnim_[games_[view_[i]].slug];
    float t = (i == sel_) ? 1.f : 0.f;
    f += (t - f) * (1 - std::exp(-dt * 14));
    return f;
  };

  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < n; ++i) {
      bool isSel = (i == sel_);
      if ((pass == 0) == isSel) continue;  // selected tile is painted last
      float cx = cxOf(i);
      if (cx + T < -20 || cx - T > W + 20) continue;
      const GameEntry& g = games_[view_[i]];
      float f = focusOf(i);
      // Edge fade so tiles slip under the margins instead of being chopped.
      drawTile(g, cx, cy, T, f, now, isSel);
      float s = T * (1 + 0.14f * f);
      hits_.push_back({cx - s / 2, cy - s / 2, s, s, i});
    }
  }
  // Soft fade at the right edge to hint there's more.
  if (cam_ < maxCam - 1) gfx_.hgrad(W - 140, 760, 140, 260, Color(8, 6, 6, 0), Color(8, 6, 6, 230));
  if (cam_ > 1) gfx_.hgrad(0, 760, kMargin, 260, Color(8, 6, 6, 230), Color(8, 6, 6, 0));
}

void App::drawHints(double now) {
  (void)now;
  const float y = 1026;
  float x = kMargin;
  PadStyle st = input_.style();
  const GameEntry* g = selected();
  if (g) {
    Primary p = primaryFor(*g);
    std::string label = p.kind == Kind::Install ? "INSTALL" : p.kind == Kind::Update ? "UPDATE" : p.kind == Kind::Installing ? "" : p.kind == Kind::Running ? "" : p.kind == Kind::Unavailable ? "" : "PLAY";
    if (!label.empty()) x += ui::prompt(gfx_, st, Action::Accept, label, x, y, 22) + 44;
    x += ui::prompt(gfx_, st, Action::X, "OPTIONS", x, y, 22) + 44;
  }
  x += ui::prompt(gfx_, st, Action::LB, "", x, y, 22) + 8;
  x += ui::prompt(gfx_, st, Action::RB, "FILTER", x, y, 22) + 44;
  x += ui::prompt(gfx_, st, Action::Y, "REFRESH", x, y, 22) + 44;
  float w = ui::promptWidth(gfx_, st, Action::Start, "MENU", 22);
  ui::prompt(gfx_, st, Action::Start, "MENU", gfx_.W() - kMargin - w, y, 22);
}

// ---- toasts ----------------------------------------------------------------------------

void App::drawToasts(double now) {
  while (!toasts_.empty() && toasts_.front().until < now) toasts_.pop_front();
  const float W = gfx_.W(), tw = 560, th = 64;
  float y = 128;
  for (auto& t : toasts_) {
    float in = ui::easeOut(static_cast<float>((now - t.born) / 0.25));
    float out = std::min(1.f, static_cast<float>((t.until - now) / 0.4));
    float a = in * out;
    float x = W - kMargin - tw + (1 - in) * 60;
    gfx_.rrect(x, y, tw, th, 6, Color(16, 12, 11, static_cast<uint8_t>(238 * a)));
    gfx_.rrectLine(x, y, tw, th, 6, 1.4f, theme::line.alpha(a));
    gfx_.rect(x, y + 8, 5, th - 16, t.color.alpha(a));
    TextStyle s = mono(16, theme::text.alpha(a), 1.4f);
    gfx_.text(gfx_.ellipsize(t.text, s, tw - 44), x + 26, y + (th - gfx_.measure("A", s).y) / 2, s);
    y += th + 10;
  }
}

// ---- menus ------------------------------------------------------------------------------

void App::drawMenu(double now) {
  if (!menu_.open) return;
  const float W = gfx_.W(), H = gfx_.H();
  float k = ui::easeOut(static_cast<float>((now - menu_.openedAt) / 0.18));
  gfx_.rect(0, 0, W, H, Color(4, 3, 3, static_cast<uint8_t>(185 * k)));

  const float pw = 720;
  const float rowH = 76;
  bool hasMsg = !menu_.message.empty();
  float msgH = 0;
  TextStyle ms = head(20, theme::dim, 0, false);
  ms.wrap = pw - 80;
  ms.lineHeight = 1.15f;
  if (hasMsg) msgH = gfx_.measure(menu_.message, ms).y + 18;
  float ph = 120 + msgH + menu_.items.size() * rowH + 74;
  float px = W / 2 - pw / 2, py = H / 2 - ph / 2 + (1 - k) * 24;

  gfx_.glow(W / 2, py + ph / 2, pw * 0.9f, ph * 0.9f, theme::magma.alpha(0.10f * k));
  gfx_.rrect(px, py, pw, ph, 12, Color(18, 14, 12, static_cast<uint8_t>(250 * k)));
  gfx_.rrectLine(px, py, pw, ph, 12, 1.5f, theme::line.alpha(k));
  ui::cornerTicks(gfx_, px - 6, py - 6, pw + 12, ph + 12, 24, 2, theme::magma.alpha(0.8f * k));

  TextStyle t = head(30, theme::text.alpha(k), 5);
  gfx_.text(gfx_.ellipsize(menu_.title, t, pw - 80), px + 40, py + 36, t);
  gfx_.rect(px + 40, py + 88, pw - 80, 1, theme::line.alpha(k));
  float y = py + 106;
  if (hasMsg) {
    ms.color = theme::dim.alpha(k);
    gfx_.text(menu_.message, px + 40, y, ms);
    y += msgH;
  }
  for (size_t i = 0; i < menu_.items.size(); ++i) {
    const MenuItem& it = menu_.items[i];
    bool on = static_cast<int>(i) == menu_.sel;
    float ry = y + i * rowH;
    if (on) {
      gfx_.hgrad(px + 16, ry, pw - 32, rowH - 8, theme::magma.alpha(0.30f * k), theme::magma.alpha(0.03f * k));
      gfx_.rect(px + 16, ry, 4, rowH - 8, (it.danger ? theme::err : theme::magma).alpha(k));
    }
    Color lc = !it.enabled ? theme::faint : it.danger ? theme::err : on ? theme::text : theme::dim;
    TextStyle l = head(24, lc.alpha(k), 3.5f);
    float ly = ry + (it.sub.empty() ? (rowH - 8 - gfx_.measure("A", l).y) / 2 : 8);
    gfx_.text(it.label, px + 44, ly, l);
    if (!it.sub.empty()) gfx_.text(gfx_.ellipsize(it.sub, mono(14, theme::faint, 1), pw - 120), px + 44, ly + 32, mono(14, (on ? theme::dim : theme::faint).alpha(k), 1));
  }
  float hy = py + ph - 52;
  float hx = px + 40;
  hx += ui::prompt(gfx_, input_.style(), Action::Accept, "SELECT", hx, hy, 22, k) + 36;
  ui::prompt(gfx_, input_.style(), Action::Back, "BACK", hx, hy, 22, k);
}

// ---- in-session -------------------------------------------------------------------------

void App::drawSession(double now) {
  bg_.draw(gfx_, now, 0.20f);
  const float W = gfx_.W(), H = gfx_.H();
  drawMark(W / 2, H * 0.28f, 84, now);
  TextStyle a = mono(16, theme::magma, 8);
  a.align = Align::Center;
  gfx_.text("NOW PLAYING", W / 2, H * 0.38f, a);
  TextStyle t = head(84, theme::text, 3);
  t.align = Align::Center;
  std::string name = gfx_.ellipsize(upper(sess_.name), t, W - 300);
  shadowText(gfx_, name, W / 2, H * 0.43f, t, 3);
  TextStyle e = mono(30, theme::ember, 6);
  e.align = Align::Center;
  gfx_.text(elapsedText(now - sess_.start), W / 2, H * 0.60f, e);

  const char* hint = "HOLD START + SELECT FOR THE VOLCANO OS MENU";
  TextStyle h = mono(15, theme::dim, 4);
  h.align = Align::Center;
  gfx_.text(hint, W / 2, H * 0.72f, h);
  if (!gameStatus_.empty()) {
    TextStyle st = head(26, theme::ember, 2, false);
    st.align = Align::Center;
    gfx_.text(gfx_.ellipsize(gameStatus_, st, W - 300), W / 2, H * 0.66f, st);
  }
  if (!overlayWin_) {  // no native overlay window on this platform: draw it here
    drawGameToasts(gfx_, now);
    if (panel_) drawPanel(gfx_, now);
  }
}

// Toasts raised by the game (or the launcher) over the top-right corner.
void App::drawGameToasts(Gfx& g, double now) {
  const float W = g.W(), cw = 470;
  float y = 56;
  for (const GameToast& t : gameToasts_) {
    bool ach = t.kind == "achievement";
    float ch = ach ? 104 : 84;
    float in = ui::easeOut(static_cast<float>((now - t.born) / 0.3));
    float out = std::min(1.f, static_cast<float>((t.until - now) / 0.4));
    float a = in * out;
    float x = W - 56 - cw + (1 - in) * 90;
    g.rrect(x, y, cw, ch, 8, Color(14, 11, 10, static_cast<uint8_t>(244 * a)));
    g.rrectLine(x, y, cw, ch, 8, 1.6f, (ach ? theme::ember : theme::magma).alpha(a));
    g.rect(x, y + 10, 5, ch - 20, (ach ? theme::ember : theme::magma).alpha(a));
    float tx = x + 28;
    if (ach) {
      g.circle(x + 54, y + ch / 2, 26, Color(64, 38, 18, static_cast<uint8_t>(255 * a)));
      g.ring(x + 54, y + ch / 2, 26, 3, theme::ember.alpha(a));
      g.line(x + 54 - 10, y + ch / 2 + 1, x + 54 - 2, y + ch / 2 + 9, 3, theme::ember.alpha(a));
      g.line(x + 54 - 2, y + ch / 2 + 9, x + 54 + 12, y + ch / 2 - 9, 3, theme::ember.alpha(a));
      tx = x + 100;
      g.text("ACHIEVEMENT UNLOCKED", tx, y + 14, mono(12, theme::ember.alpha(a), 3));
    }
    TextStyle ts = head(22, theme::text.alpha(a), 2);
    g.text(g.ellipsize(t.title, ts, cw - (tx - x) - 24), tx, y + (ach ? 36 : 14), ts);
    TextStyle bs = head(17, theme::dim.alpha(a), 0, false);
    g.text(g.ellipsize(t.body, bs, cw - (tx - x) - 24), tx, y + (ach ? 68 : 48), bs);
    y += ch + 12;
  }
}

// The in-game Volcano OS menu.
void App::drawPanel(Gfx& g, double now) {
  const float W = g.W();
  float k = ui::easeOut(static_cast<float>((now - panelOpened_) / 0.18));
  bool native = sess_.kind == SessionKind::Native;
  const float pw = 600;
  float items = native ? 3.f : 2.f;
  float ph = 250 + items * 74 + (panelFriends_.empty() ? 0 : 60 + std::min<size_t>(5, panelFriends_.size()) * 40) + 70;
  float px = 56 - (1 - k) * 60, py = 56;
  (void)W;
  g.rrect(px, py, pw, ph, 12, Color(14, 11, 10, static_cast<uint8_t>(246 * k)));
  g.rrectLine(px, py, pw, ph, 12, 1.6f, theme::magma.alpha(k));
  ui::cornerTicks(g, px - 6, py - 6, pw + 12, ph + 12, 22, 2, theme::magma.alpha(k));

  drawMarkOn(g, px + 56, py + 52, 30, now);
  TextStyle t1 = head(24, theme::text.alpha(k), 6);
  float vw = g.text("VOLCANO ", px + 100, py + 24, t1).x;
  t1.color = theme::magma.alpha(k);
  g.text("OS", px + 100 + vw, py + 24, t1);
  TextStyle gs = mono(14, theme::dim.alpha(k), 3);
  g.text(g.ellipsize(upper(sess_.name), gs, pw - 140), px + 100, py + 62, gs);
  g.rect(px + 30, py + 100, pw - 60, 1, theme::line.alpha(k));

  char tbuf[16];
  int s = static_cast<int>(now - sess_.start);
  std::snprintf(tbuf, sizeof tbuf, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  g.text("SESSION", px + 36, py + 118, mono(12, theme::faint.alpha(k), 4));
  g.text(tbuf, px + 36, py + 138, head(28, theme::ember.alpha(k), 3));
  if (!gameStatus_.empty()) {
    TextStyle st = head(18, theme::dim.alpha(k), 0, false);
    st.align = Align::Right;
    g.text(g.ellipsize(gameStatus_, st, pw - 280), px + pw - 36, py + 146, st);
  }

  const char* labels3[3] = {"RESUME GAME", "SWITCH TO LAUNCHER", quitArmed_ ? "PRESS AGAIN TO QUIT" : "QUIT GAME"};
  const char* labels2[2] = {"RESUME GAME", "EXIT TO LAUNCHER"};
  float y = py + 190;
  for (int i = 0; i < static_cast<int>(items); ++i) {
    bool on = i == panelSel_;
    bool danger = (native && i == 2) || (!native && i == 1);
    Color accent = danger ? theme::err : theme::magma;
    if (on) {
      g.hgrad(px + 14, y, pw - 28, 66, accent.alpha(0.32f * k), accent.alpha(0.03f * k));
      g.rect(px + 14, y, 4, 66, accent.alpha(k));
    }
    TextStyle l = head(22, (danger ? (on ? theme::err : theme::dim) : (on ? theme::text : theme::dim)).alpha(k), 3.5f);
    g.text(native ? labels3[i] : labels2[i], px + 40, y + (66 - g.measure("A", l).y) / 2, l);
    y += 74;
  }
  if (!panelFriends_.empty()) {
    y += 6;
    g.text("FRIENDS ONLINE", px + 36, y, mono(12, theme::faint.alpha(k), 4));
    y += 30;
    for (size_t i = 0; i < panelFriends_.size() && i < 5; ++i) {
      g.circle(px + 44, y + 14, 5, theme::ok.alpha(k));
      TextStyle fn = head(18, theme::text.alpha(k), 1.5f);
      float w = g.text(g.ellipsize(upper(panelFriends_[i].first), fn, 240), px + 62, y + 2, fn).x;
      TextStyle fs = mono(12, theme::faint.alpha(k), 1);
      g.text(g.ellipsize(upper(panelFriends_[i].second), fs, pw - 140 - w), px + 62 + w + 14, y + 6, fs);
      y += 40;
    }
  }
  float hx = px + 36, hy = py + ph - 52;
  hx += ui::prompt(g, input_.style(), Action::Accept, "SELECT", hx, hy, 22, k) + 34;
  ui::prompt(g, input_.style(), Action::Back, "RESUME", hx, hy, 22, k);
}

}  // namespace vos
