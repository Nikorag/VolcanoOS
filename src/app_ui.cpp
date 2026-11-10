// Everything that draws: boot, sign-in, the Home shelf, menus, toasts.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app.hpp"
#include "widgets.hpp"

namespace vos {

namespace {

constexpr float kMargin = 96.f;
constexpr float kHome = 72.f;  // Home / top-bar side margin

TextStyle mono(float size, Color c, float tracking = 0) {
  TextStyle t;
  t.font = Font::Mono;
  t.size = size;
  t.color = c;
  t.tracking = tracking * 0.3f;  // Figtree reads best nearly unspaced
  return t;
}
TextStyle head(float size, Color c, float tracking = 0, bool bold = true) {
  TextStyle t;
  t.font = Font::Head;
  t.size = size;
  t.color = c;
  t.tracking = tracking * 0.3f;  // Figtree reads best nearly unspaced
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

// "SWITCH TO FULLSCREEN" -> "Switch to fullscreen" (keeps brand and acronym casing).
std::string sentence(std::string s) {
  bool lowerAny = false;
  for (char c : s) if (c >= 'a' && c <= 'z') lowerAny = true;
  if (lowerAny) return s;  // already mixed case (e.g. a game name)
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  auto rep = [&](const char* from, const char* to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + 1)) s.replace(p, std::string(from).size(), to);
  };
  rep("volcano os", "Volcano OS"); rep("wi-fi", "Wi-Fi"); rep("zonto", "Zonto");
  return s;
}

void shadowText(Gfx& g, const std::string& s, float x, float y, TextStyle st, float off = 3) {
  TextStyle sh = st;
  sh.color = Color(0, 0, 0, static_cast<uint8_t>(st.color.a * 0.65f));
  g.text(s, x + off, y + off, sh);
  g.text(s, x, y, st);
}

void wifiIconDraw(Gfx& g, float cx, float cy, Color c) {
  const float up = -1.5708f, span = 0.78f;
  for (int i = 0; i < 3; ++i) g.arc(cx, cy + 9, 6.f + i * 6.5f, 2.2f, up - span, up + span, c);
  g.circle(cx, cy + 9, 1.8f, c);
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
  // `s` is the old mark's nominal size; the real logo is drawn ~1.5x that tall.
  float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(now) * 2.1f);
  g.glow(cx, cy, s * 1.5f, s * 1.1f, Color(255, 100, 30, static_cast<uint8_t>(60 * pulse)));
  g.logo(cx, cy, s * 1.5f);
}

// ---- frame dispatch ---------------------------------------------------------------

void App::frame(double now, float dt) {
  gfx_.begin();

  if (screen_ == Screen::SignIn) {
    // Phone (device-code) sign-in.
    switch (device_.state()) {
      case DeviceSignIn::State::Done: {
        Credentials c = device_.credentials();
        device_.reset();
        onSignedIn(c);
        break;
      }
      case DeviceSignIn::State::Failed:
        signInError_ = upper(device_.error());
        device_.reset();
        break;
      case DeviceSignIn::State::Unsupported:
        signInError_ = "THIS SERVER DOES NOT SUPPORT PHONE SIGN-IN";
        device_.reset();
        break;
      case DeviceSignIn::State::Cancelled:
        device_.reset();
        break;
      default:
        break;
    }
    // Browser (authorization code + PKCE) sign-in.
    auto st = signin_.state();
    if (st == SignInFlow::State::Done) {
      Credentials c = signin_.credentials();
      signin_.cancel();
      onSignedIn(c);
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
      case Screen::System: drawSystem(now); break;
      case Screen::App:
        if (activeApp_) activeApp_->draw(now, dt, input_.style());
        break;
    }
  }
  renderOverlay(now);
  drawControlCenter(now);
  drawMenu(now);
  osk_.draw(gfx_, input_.style(), now);
  drawToasts(now);

  // Screen fade-in.
  float f = ui::easeOut(static_cast<float>((now - screenSince_) / 0.5));
  if (f < 1) gfx_.rect(0, 0, gfx_.W(), gfx_.H(), Color(0, 0, 0, static_cast<uint8_t>((1 - f) * 255)));
  gfx_.present();
}

// ---- boot / sign-in ---------------------------------------------------------------

// Console brand: ZONTO in white, VOLCANO in magma, with the software name beneath.
static void wordmark(Gfx& g, float cx, float y, float size, double now, bool tag) {
  TextStyle a = head(size, theme::text, size * 0.16f);
  Vec2 m = g.measure("ZONTO VOLCANO", a);
  float x0 = cx - m.x / 2;
  float zw = g.measure("ZONTO ", a).x;
  g.text("ZONTO ", x0, y, a);
  TextStyle o = a;
  o.color = theme::magma;
  g.text("VOLCANO", x0 + zw, y, o);
  if (tag) {
    TextStyle t = mono(size * 0.19f, theme::text, size * 0.06f);
    t.align = Align::Center;
    g.text("VOLCANO OS", cx, y + m.y + size * 0.10f, t);
    TextStyle u = mono(size * 0.15f, theme::dim, size * 0.045f);
    u.align = Align::Center;
    g.text("THE STANDALONE CONSOLE FOR ZONTO GAMES", cx, y + m.y + size * 0.10f + size * 0.34f, u);
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

void App::drawDeviceSignIn(double now) {
  bg_.draw(gfx_, now, 0.05f);
  const float W = gfx_.W(), H = gfx_.H();
  drawMark(W / 2, H * 0.115f, 56, now);
  wordmark(gfx_, W / 2, H * 0.165f, 54, now, false);

  const auto code = device_.code();
  const bool ready = device_.state() == DeviceSignIn::State::Waiting && !code.userCode.empty();
  const float pw = 1120, ph = 520, px = W / 2 - pw / 2, py = H * 0.30f;
  gfx_.rrect(px, py, pw, ph, 10, Color(14, 11, 10, 220));
  gfx_.rrectLine(px, py, pw, ph, 10, 1.5f, theme::line);
  ui::cornerTicks(gfx_, px - 6, py - 6, pw + 12, ph + 12, 22, 2, theme::magma.alpha(0.7f));

  const PadStyle ps = input_.style();
  const float hintY = py + ph - 56;
  float hx = px + 60;
  hx += ui::prompt(gfx_, ps, Action::Back, "CANCEL", hx, hintY, 22) + 48;

  if (!ready) {
    gfx_.arc(W / 2, py + ph / 2 - 20, 24, 4, static_cast<float>(now) * 4, static_cast<float>(now) * 4 + 4.2f, theme::magma);
    TextStyle t = head(26, theme::text, 5);
    t.align = Align::Center;
    gfx_.text("GETTING YOUR CODE", W / 2, py + ph / 2 + 24, t);
  } else {

    // QR of the prefilled verification URL, 1 px per module so scaling stays crisp.
    if (code.verificationUrlComplete != qrFor_) {
      if (qrTex_) SDL_DestroyTexture(qrTex_);
      qrTex_ = nullptr;
      qrFor_ = code.verificationUrlComplete;
      QrCode q = encodeQr(qrFor_, true);
      if (q.size == 0) q = encodeQr(qrFor_, false);
      if (q.size > 0) {
        const int quiet = 4, n = q.size + quiet * 2;
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, n, n, 32, SDL_PIXELFORMAT_RGBA32)) {
          SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, 255, 255, 255, 255));
          const Uint32 black = SDL_MapRGBA(s->format, 0, 0, 0, 255);
          for (int y = 0; y < q.size; ++y)
            for (int x = 0; x < q.size; ++x)
              if (q.dark(x, y)) static_cast<Uint32*>(s->pixels)[(y + quiet) * (s->pitch / 4) + (x + quiet)] = black;
          qrTex_ = gfx_.makeTexture(s);
          if (qrTex_) SDL_SetTextureScaleMode(qrTex_, SDL_ScaleModeNearest);
          SDL_FreeSurface(s);
        }
      }
    }
    const float qs = 380, qx = px + 60, qy = py + 54;
    gfx_.rrect(qx - 6, qy - 6, qs + 12, qs + 12, 8, Color(255, 255, 255, 255));
    if (qrTex_) gfx_.image(qrTex_, qx, qy, qs, qs);

    const float rx = qx + qs + 60, rw = px + pw - 60 - rx;
    gfx_.text("SIGN IN ON YOUR PHONE", rx, py + 54, head(30, theme::text, 4));
    gfx_.text("SCAN THE CODE, OR GO TO", rx, py + 122, mono(14, theme::faint, 3));
    std::string url = code.verificationUrl;
    if (url.rfind("https://", 0) == 0) url = url.substr(8);
    TextStyle us = mono(22, theme::ember, 1);
    gfx_.text(gfx_.ellipsize(url, us, rw), rx, py + 148, us);
    gfx_.text("AND ENTER THIS CODE", rx, py + 214, mono(14, theme::faint, 3));
    TextStyle cs = head(72, theme::text, 8);
    gfx_.glow(rx + 150, py + 280, 260, 60, theme::magma.alpha(0.10f));
    gfx_.text(code.userCode, rx, py + 238, cs);

    const double left = code.expiresAt - now;
    char buf[48];
    if (left > 0) std::snprintf(buf, sizeof buf, "CODE EXPIRES IN %d:%02d", static_cast<int>(left) / 60, static_cast<int>(left) % 60);
    else std::snprintf(buf, sizeof buf, "GETTING A NEW CODE");
    gfx_.text(buf, rx, py + 346, mono(14, theme::faint, 3));
    gfx_.arc(rx + 10, py + 410, 10, 3, static_cast<float>(now) * 4, static_cast<float>(now) * 4 + 4.2f, theme::magma);
    gfx_.text("WAITING FOR APPROVAL", rx + 34, py + 400, mono(15, theme::dim, 3));
  }

  gfx_.rect(kMargin, H - 74, W - kMargin * 2, 1, theme::line.alpha(0.6f));
  gfx_.text("WWW.NIKORAG.CO.UK", kMargin, H - 52, mono(14, theme::faint, 3));
  float w = ui::promptWidth(gfx_, ps, Action::Start, "NETWORK & MENU", 22);
  ui::prompt(gfx_, ps, Action::Start, "NETWORK & MENU", W - kMargin - w, H - 56, 22);
}

void App::drawSignIn(double now) {
  const auto ds = device_.state();
  if (ds == DeviceSignIn::State::Starting || ds == DeviceSignIn::State::Waiting) return drawDeviceSignIn(now);

  bg_.draw(gfx_, now, 0.05f);
  float W = gfx_.W(), H = gfx_.H();
  drawMark(W / 2, H * 0.26f, 90, now);
  wordmark(gfx_, W / 2, H * 0.36f, 92, now, true);

  auto st = signin_.state();
  float py = H * 0.62f;
  float pw = 760, ph = 270, px = W / 2 - pw / 2;
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
      gfx_.text("Scan a code with your phone. No keyboard needed.", W / 2, py + 84, sub);
    }
    // Button sized to its label (as on Home), with no key glyph drawn over it: the keys are listed below.
    const std::string label = api_.hasToken() ? "TRY AGAIN" : "SIGN IN WITH YOUR PHONE";
    const float bw = std::max(380.f, 120.f + gfx_.measure(label, head(66 * 0.40f, theme::text, 4)).x);
    ui::button(gfx_, W / 2 - bw / 2, py + 124, bw, 66, label, theme::magma, true, now);
    std::vector<std::pair<Action, std::string>> keys = {{Action::Accept, api_.hasToken() ? "TRY AGAIN" : "SIGN IN"}};
    float total = 0;
    for (auto& k : keys) total += ui::promptWidth(gfx_, input_.style(), k.first, k.second, 20) + 44;
    float kx = W / 2 - (total - 44) / 2;
    for (auto& k : keys) kx += ui::prompt(gfx_, input_.style(), k.first, k.second, kx, py + 214, 20) + 44;
  }

  // Footer.
  gfx_.rect(kMargin, H - 74, W - kMargin * 2, 1, theme::line.alpha(0.6f));
  TextStyle f = mono(14, theme::faint, 3);
  gfx_.text("WWW.NIKORAG.CO.UK", kMargin, H - 52, f);
  float x = W - kMargin;
  float w = ui::promptWidth(gfx_, input_.style(), Action::Start, "NETWORK & MENU", 22);
  ui::prompt(gfx_, input_.style(), Action::Start, "NETWORK & MENU", x - w, H - 56, 22);
}

// ---- home ----------------------------------------------------------------------------

void App::drawHome(double now, float dt) {
  bg_.draw(gfx_, now, 0.18f);
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
  // Scrims so text always reads, whatever the art: dark on the left, fading out by ~80% width.
  {
    const float W = gfx_.W();
    gfx_.hgrad(0, 0, W * 0.30f, gfx_.H(), Color(14, 11, 10, 255), Color(14, 11, 10, 240));
    gfx_.hgrad(W * 0.30f, 0, W * 0.28f, gfx_.H(), Color(14, 11, 10, 240), Color(14, 11, 10, 100));
    gfx_.hgrad(W * 0.58f, 0, W * 0.22f, gfx_.H(), Color(14, 11, 10, 100), Color(14, 11, 10, 0));
    gfx_.vgrad(0, gfx_.H() - 460, W, 460, Color(14, 11, 10, 0), Color(14, 11, 10, 255));
  }

  drawTopBar(now);
  if (g) drawInfo(g, now);
  else {
    TextStyle t = head(54, theme::dim, 4);
    gfx_.text(filter_ == Filter::All ? "NO GAMES YET" : "Nothing here", kHome, 300, t);
    TextStyle s = mono(18, theme::faint, 2);
    gfx_.text(filter_ == Filter::All ? "Open the menu and visit the store to add some." : "Try another filter with LB / RB.", kHome, 380, s);
  }
  drawShelf(now, dt);
  drawHints(now);
}

void App::drawTopBar(double now) {
  const float W = gfx_.W(), cy = 64;
  // Scrim so the bar reads over any hero art.
  gfx_.vgrad(0, 0, W, 170, Color(14, 11, 10, 215), Color(14, 11, 10, 0));
  auto pill = [&](float x0, float x1) {
    gfx_.rrect(x0, cy - 28, x1 - x0, 56, 28, Color(28, 22, 19, 175));
    gfx_.rrectLine(x0, cy - 28, x1 - x0, 56, 28, 1.2f, theme::line);
  };
  drawMark(kHome + 20, cy + 1, 38, now);
  TextStyle wm = head(22, theme::text, 10);
  gfx_.text("ZONTO", kHome + 54, cy - gfx_.measure("ZONTO", wm).y / 2, wm);
  const float navX = kHome + 54 + gfx_.measure("ZONTO", wm).x + 40;

  // Tabs with shoulder glyphs: library filters on Home, pages elsewhere.
  const bool onPages = screen_ == Screen::Pages;
  const bool onSystem = screen_ == Screen::System;
  const char* labels[4] = {"All", "Installed", "Web", "Apps"};
  const int nTabs = (onPages || onSystem) ? 3 : kFilters;
  if (onPages || onSystem)
    for (int i = 0; i < 3; ++i) labels[i] = onSystem ? SystemPages::label(i) : Pages::label(i);
  const int activeTab = onSystem ? static_cast<int>(sys_->tab()) : onPages ? static_cast<int>(pages_->tab()) : static_cast<int>(filter_);
  const PadStyle ps = input_.style();

  auto tabs = [&](bool draw) {
    float x = navX + 6;
    for (int i = 0; i < nTabs; ++i) {
      bool on = activeTab == i;
      TextStyle t = head(19, on ? Color(22, 15, 11) : theme::dim, 0, on);
      if (!on) t.bold = false;
      Vec2 m = gfx_.measure(labels[i], t);
      float bw = m.x + 44;
      if (draw) {
        if (on) gfx_.rrect(x, cy - 22, bw, 44, 22, theme::text);
        if (on && screen_ == Screen::Home && homeFocus_ == 0) gfx_.rrectLine(x - 3, cy - 25, bw + 6, 50, 25, 3, Color(255, 255, 255));
        gfx_.text(labels[i], x + 22, cy - m.y / 2, t);
      }
      x += bw + 6;
    }
    return x - 6;
  };
  pill(navX, tabs(false) + 6);
  tabs(true);

  // Right cluster: device dot, offline chip, wifi, clock, player.
  const std::string clock = clockText();
  TextStyle c = head(24, theme::text, 0);
  const std::string tag = player_.gamertag;
  float rx = W - kHome;
  if (!tag.empty()) {
    TextStyle n = head(18, theme::text, 0);
    TextStyle lv = head(14, Color(189, 178, 170), 0, false);
    std::string lvl = "Level " + std::to_string(player_.level);
    float tw = std::max(gfx_.measure(tag, n).x, gfx_.measure(lvl, lv).x);
    float pw = 20 + tw + 14 + 48 + 6;
    float px = rx - pw;
    pill(px, rx);
    n.align = Align::Right;
    lv.align = Align::Right;
    gfx_.text(tag, px + 20 + tw, cy - 22, n);
    gfx_.text(lvl, px + 20 + tw, cy + 2, lv);
    float ax = rx - 6 - 24;
    gfx_.circle(ax, cy, 24, theme::magma);
    gfx_.circle(ax, cy, 20, Color(42, 26, 18));
    TextStyle in = head(19, Color(255, 138, 85), 0);
    in.align = Align::Center;
    std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(tag[0]))));
    gfx_.text(ini, ax, cy - gfx_.measure(ini, in).y / 2, in);
    rx = px - 28;
  }
  Vec2 cm = gfx_.measure(clock, c);
  rx -= cm.x;
  gfx_.text(clock, rx, cy - cm.y / 2, c);
  rx -= 28 + 26;
  bool pad = input_.hasPad();
  wifiIconDraw(gfx_, rx + 13, cy - 9, offline_ ? theme::warn : theme::text);
  rx -= 8;
  if (pad) {
    gfx_.circle(rx - 4, cy, 5, theme::ok);
    gfx_.glow(rx - 4, cy, 14, 14, theme::ok.alpha(0.35f));
  }
}

// Header for full-screen sub-pages: logo and title on the left, clock and avatar on the right.
void App::drawTitleBar(const std::string& title) {
  const float W = gfx_.W(), cy = 62;
  gfx_.vgrad(0, 0, W, 150, Color(14, 11, 10, 215), Color(14, 11, 10, 0));
  drawMarkOn(gfx_, kHome + 20, cy + 1, 38, nowSeconds());
  TextStyle ts = head(34, theme::text, 0);
  gfx_.text(title, kHome + 58, cy - gfx_.measure(title, ts).y / 2, ts);
  float rx = W - kHome;
  const std::string tag = player_.gamertag;
  if (!tag.empty()) {
    gfx_.circle(rx - 26, cy, 26, theme::magma);
    gfx_.circle(rx - 26, cy, 22, Color(42, 26, 18));
    TextStyle in = head(20, Color(255, 138, 85), 0);
    in.align = Align::Center;
    std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(tag[0]))));
    gfx_.text(ini, rx - 26, cy - gfx_.measure(ini, in).y / 2, in);
    rx -= 52 + 28;
  }
  TextStyle c = head(24, theme::text, 0);
  const std::string clock = clockText();
  rx -= gfx_.measure(clock, c).x;
  gfx_.text(clock, rx, cy - gfx_.measure(clock, c).y / 2, c);
}

void App::drawSystem(double now) {
  bg_.draw(gfx_, now, 0.62f);
  sys_->update(now);
  drawTitleBar("Settings");
  sys_->draw(now, input_.style());
}

void App::drawPages(double now) {
  bg_.draw(gfx_, now, 0.62f);
  drawTopBar(now);
  pages_->draw(now, 0.016f, input_.style());
}

void App::drawHero(const GameEntry* g, double now) {
  const float W = gfx_.W();
  const float hw = 1340, hx = W - hw, hh = gfx_.H();
  float k = ui::easeOut(static_cast<float>((now - heroSince_) / 0.6));

  auto one = [&](const std::string& u, float a) {
    if (a <= 0 || u.empty()) return;
    if (const ArtImage* img = art_->get(u))
      gfx_.cover(img->tex, img->w, img->h, hx, 0, hw, hh, a * art_->fade(img, now), 0.f);
  };
  one(heroPrev_, 1 - k);
  one(heroCur_, k);

  // No hero art: show the logo large and soft instead of an empty hole.
  if (g && g->heroUrl.empty()) {
    if (const ArtImage* logo = art_->get(g->logoUrl)) {
      float s = 420;
      gfx_.glow(W * 0.74f, 380, 380, 300, ui::tint(g->slug).alpha(0.22f));
      gfx_.contain(logo->tex, logo->w, logo->h, W * 0.74f - s / 2, 380 - s / 2, s, s, 0.9f * art_->fade(logo, now));
    } else if (g) {
      gfx_.glow(W * 0.74f, 380, 420, 320, ui::tint(g->slug).alpha(0.22f));
    }
  }
}

namespace {

// Glass card: the translucent warm-black plate with a hairline border used all over the console UI.
void glass(Gfx& g, float x, float y, float w, float h, float r, float alpha = 1.f) {
  g.rrect(x, y, w, h, r, Color(28, 22, 19, static_cast<uint8_t>(190 * alpha)));
  g.rrectLine(x, y, w, h, r, 1.2f, theme::line.alpha(alpha));
}

void clockIcon(Gfx& g, float cx, float cy, float r, Color c) {
  g.ring(cx, cy, r, 2, c);
  g.line(cx, cy - r * 0.55f, cx, cy, 2, c);
  g.line(cx, cy, cx + r * 0.42f, cy + r * 0.25f, 2, c);
}

void trophyIcon(Gfx& g, float cx, float cy, float r, Color c) {
  g.rrectLine(cx - r * 0.55f, cy - r, r * 1.1f, r * 1.2f, r * 0.45f, 2, c);
  g.line(cx, cy + r * 0.2f, cx, cy + r * 0.7f, 2, c);
  g.line(cx - r * 0.5f, cy + r * 0.8f, cx + r * 0.5f, cy + r * 0.8f, 2, c);
}


}  // namespace

void App::drawInfo(const GameEntry* g, double now) {
  const float x0 = kHome;
  float slide = ui::easeOut(infoSlide_);
  float ox = (1 - slide) * -36, a = slide;
  Primary prim = primaryFor(*g);
  const Installed* inst = installer_->installed(g->slug);

  // Genre line.
  std::string tags = g->tags.empty() ? (g->isWeb() ? "Web game" : "Game") : g->tags;
  gfx_.text(tags, x0 + ox, 430, head(21, Color(212, 200, 191).alpha(a), 0, false));

  // Title, shrunk to fit one line.
  std::string title = g->name;
  const float maxW = 860;
  float size = 92;
  TextStyle ts = head(size, theme::text.alpha(a), 0);
  while (size > 48 && gfx_.measure(title, ts).x > maxW) {
    size -= 4;
    ts.size = size;
  }
  title = gfx_.ellipsize(title, ts, maxW);
  Vec2 tm = gfx_.measure(title, ts);
  const float ty = 466;
  shadowText(gfx_, title, x0 + ox, ty, ts, 3);

  // Blurb, capped at three lines.
  TextStyle bs = head(23, Color(217, 206, 198).alpha(a), 0, false);
  bs.wrap = 700;
  bs.lineHeight = 1.4f;
  std::string blurb = g->blurb.empty() ? "No description yet." : clampText(g->blurb, 170);
  Vec2 bm = gfx_.measure(blurb, bs);
  const float by0 = ty + tm.y + 10;
  gfx_.text(blurb, x0 + ox, by0, bs);

  // Primary action row.
  const float ay = std::min(by0 + bm.y + 26, 724.f);
  if (prim.kind == Kind::Installing) {
    auto j = installer_->job(g->slug);
    float frac = j ? j->fraction() : 0;
    std::string phase = j ? j->phaseLabel() : "WORKING";
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(frac * 100));
    gfx_.text(phase, x0 + ox, ay - 6, head(24, theme::ember.alpha(a), 1));
    std::string detail = std::string(pct);
    if (j && j->phase.load() == InstallJob::Downloading)
      detail += "   " + humanBytes(j->done.load()) + " / " + humanBytes(j->total.load());
    gfx_.text(detail, x0 + ox, ay + 26, mono(20, theme::text.alpha(a), 0));
    ui::progressBar(gfx_, x0 + ox, ay + 62, 620, 10, frac, theme::magma);
    gfx_.glow(x0 + ox + 620 * frac, ay + 67, 40, 22, theme::magma.alpha(0.4f));
  } else {
    const float bh = 76;
    const PadStyle ps = input_.style();
    TextStyle ls = head(26, Color(26, 14, 8), 0);
    float bw = 52 + gfx_.measure(prim.label, ls).x + 52;
    float bx = x0 + ox;
    Color accent = prim.enabled ? theme::magma : theme::panelHi;
    if (prim.enabled) {
      float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 3.2f);
      gfx_.glow(bx + bw / 2, ay + bh / 2 + 10, bw * 0.8f, bh * 1.5f, theme::magma.alpha((0.30f + 0.10f * pulse) * a));
    }
    gfx_.rrect(bx, ay, bw, bh, bh / 2, accent.alpha(a));
    if (homeFocus_ == 2 && homeAct_ == 0) gfx_.rrectLine(bx - 4, ay - 4, bw + 8, bh + 8, bh / 2 + 4, 4, Color(255, 255, 255, static_cast<uint8_t>(255 * a)));
    ls.color = (prim.enabled ? Color(26, 14, 8) : theme::faint).alpha(a);
    gfx_.lintMute(1);
    gfx_.text(prim.label, bx + 52, ay + (bh - gfx_.measure(prim.label, ls).y) / 2, ls);
    gfx_.lintMute(-1);
    gfx_.lintBox(Gfx::Box::Button, bx, ay, bw, bh, prim.label);
    // Options: a round button with three dots.
    float ox2 = bx + bw + 24, cx = ox2 + bh / 2, cy = ay + bh / 2;
    glass(gfx_, ox2, ay, bh, bh, bh / 2, a);
    if (homeFocus_ == 2 && homeAct_ == 1) gfx_.rrectLine(ox2 - 4, ay - 4, bh + 8, bh + 8, bh / 2 + 4, 4, Color(255, 255, 255, static_cast<uint8_t>(255 * a)));
    for (int i = -1; i <= 1; ++i) gfx_.circle(cx + i * 11, cy, 3, theme::text.alpha(a));
  }

  // Stat cards.
  struct Card { std::string icon, label, value; Color c; float bar; };
  Card c0{"clock", "Time played", humanPlaytime(g->minutesPlayed), theme::text, -1};
  Card c1{"trophy", "Achievements", g->achTotal ? std::to_string(g->achUnlocked) + " of " + std::to_string(g->achTotal) : "None", theme::text,
          g->achTotal ? static_cast<float>(g->achUnlocked) / g->achTotal : -1};
  std::string status, sub = "Status";
  Color sc = theme::text;
  if (inst && g->hasBuild && inst->version != g->build.version) { status = "Update ready"; sc = theme::ember; sub = humanBytes(g->build.sizeBytes) + " download"; }
  else if (prim.kind == Kind::Running) { status = "Playing now"; sc = theme::ok; sub = "Running"; }
  else if (inst) { status = "Ready to play"; sc = theme::ok; sub = "Installed, v" + inst->version; }
  else if (g->hasBuild) { status = "Not installed"; sub = humanBytes(g->build.sizeBytes) + " download"; }
  else if (g->isWeb()) { status = "Ready to play"; sc = theme::ok; sub = "Plays in the browser"; }
  else { status = "Unavailable"; sc = theme::faint; sub = "Not available on this device"; }
  if (isAppSlug(g->slug)) {
    AppModule* app = appFor(g->slug);
    const std::string st = app ? app->status() : "";
    c0 = Card{"", "Account", st.empty() ? "Not set up" : st, st.empty() ? theme::dim : theme::text, -1};
    c1 = Card{"", "Type", "App", theme::text, -1};
    status = prim.kind == Kind::Running ? "Playing now" : "Ready";
    sub = "Status";
    sc = theme::ok;
  }
  Card c2{"", sub, status, sc, -1};

  const float cy0 = 818, cw = 300, ch = 136;
  Card cards[3] = {c0, c1, c2};
  for (int i = 0; i < 3; ++i) {
    float cx = x0 + ox + i * (cw + 20);
    glass(gfx_, cx, cy0, cw, ch, 26, a);
    const Card& c = cards[i];
    float lx = cx + 26;
    if (c.icon == "clock") { clockIcon(gfx_, lx + 10, cy0 + 34, 9, theme::dim.alpha(a)); lx += 30; }
    else if (c.icon == "trophy") { trophyIcon(gfx_, lx + 10, cy0 + 33, 10, theme::dim.alpha(a)); lx += 30; }
    gfx_.text(c.label, lx, cy0 + 22, head(18, theme::dim.alpha(a), 0, false));
    bool small = c.icon.empty();
    TextStyle v = head(small ? 30 : 34, c.c.alpha(a), 0);
    float vy = cy0 + ch - 26 - gfx_.measure(c.value, v).y * (c.bar >= 0 ? 1.55f : 1.0f);
    gfx_.text(gfx_.ellipsize(c.value, v, cw - 52), cx + 26, c.bar >= 0 ? cy0 + 62 : cy0 + ch - 26 - gfx_.measure(c.value, v).y, v);
    (void)vy;
    if (c.bar >= 0) ui::progressBar(gfx_, cx + 26, cy0 + ch - 30, cw - 52, 6, c.bar, theme::magma, Color(247, 241, 236, 40));
  }
}

void App::drawBadge(const GameEntry& g, float cx, float cy, float r, double now) {
  Primary p = primaryFor(g);
  if (p.kind == Kind::PlayWeb || p.kind == Kind::Unavailable) return;
  switch (p.kind) {
    case Kind::Running: {
      float k = 0.6f + 0.4f * std::sin(static_cast<float>(now) * 5);
      gfx_.circle(cx, cy, r, Color(14, 11, 10, 200));
      gfx_.circle(cx, cy, r * 0.5f, theme::ok.alpha(k));
      break;
    }
    case Kind::Installing: {
      auto j = installer_->job(g.slug);
      float f = j ? j->fraction() : 0;
      gfx_.circle(cx, cy, r, Color(14, 11, 10, 200));
      gfx_.ring(cx, cy, r - 3, 3, theme::line);
      gfx_.arc(cx, cy, r - 3, 3, -1.5708f, -1.5708f + 6.2831f * std::max(0.03f, f), theme::magma);
      break;
    }
    case Kind::Update:
      gfx_.circle(cx, cy, r, theme::magma);
      arrow(gfx_, cx, cy, r * 0.42f, true, Color(26, 14, 8));
      break;
    case Kind::Install:
      gfx_.circle(cx, cy, r, Color(14, 11, 10, 200));
      arrow(gfx_, cx, cy, r * 0.42f, false, theme::text);
      break;
    case Kind::Play:
      gfx_.circle(cx, cy, r, Color(14, 11, 10, 200));
      gfx_.line(cx - r * 0.38f, cy + 1, cx - r * 0.1f, cy + r * 0.32f, 2.6f, theme::ok);
      gfx_.line(cx - r * 0.1f, cy + r * 0.32f, cx + r * 0.42f, cy - r * 0.3f, 2.6f, theme::ok);
      break;
    default: break;
  }
}

// One library tile: rounded art, a white ring and warm glow when focused. `s` is its current size.
void App::drawTile(const GameEntry& g, float x, float y, float s, float focus, double now, bool selected) {
  const float r = s * 0.22f;
  if (focus > 0.02f) gfx_.glow(x + s / 2, y + s / 2 + 14, s * 0.95f, s * 0.8f, Color(0, 0, 0, 120).alpha(focus));
  // Fill the whole tile with art: the wide key art when there is some, else the logo image.
  const ArtImage* fill = g.heroUrl.empty() ? nullptr : art_->get(g.heroUrl, 512);
  if (!fill) fill = art_->get(g.logoUrl, 512);
  if (fill) {
    gfx_.rrectGrad(x, y, s, s, r, Color(46, 36, 30), Color(30, 23, 19));
    gfx_.imageRound(fill->tex, fill->w, fill->h, x, y, s, s, r, art_->fade(fill, now));
  } else {
    ui::placeholder(gfx_, x, y, s, s, g.slug);
  }
  if (selected) {
    if (homeFocus_ == 1) {
      gfx_.rrectLine(x - 4, y - 4, s + 8, s + 8, r + 4, 4, Color(255, 255, 255));
      gfx_.rrectLine(x - 12, y - 12, s + 24, s + 24, r + 12, 8, theme::magma.alpha(0.30f));
    } else {
      gfx_.rrectLine(x - 3, y - 3, s + 6, s + 6, r + 3, 3, theme::magma.alpha(0.55f));
    }
  } else {
    gfx_.rrectLine(x, y, s, s, r, 1.2f, theme::line);
  }
  if (g.isWeb()) {
    TextStyle w = mono(14, theme::text, 0);
    float cw = gfx_.measure("Web", w).x + 22;
    gfx_.rrect(x + 12, y + s - 40, cw, 28, 14, Color(14, 11, 10, 200));
    gfx_.text("Web", x + 23, y + s - 40 + (28 - gfx_.measure("Web", w).y) / 2, w);
  }
  drawBadge(g, x + s - 29, y + 29, 17, now);
}

void App::drawShelf(double now, float dt) {
  const float W = gfx_.W();
  const float cy = 236;  // the row's centre line
  const float base = 136, big = 208, gap = 20, textW = 262;
  int n = static_cast<int>(view_.size());

  hits_.clear();
  if (n == 0) return;

  // Animated focus per tile; widths follow it so neighbours slide aside.
  std::vector<float> f(n);
  for (int i = 0; i < n; ++i) {
    float& v = focusAnim_[games_[view_[i]].slug];
    v += ((i == sel_ ? 1.f : 0.f) - v) * (1 - std::exp(-dt * 14));
    f[i] = v;
  }
  auto widthOf = [&](int i) { return base + (big - base) * f[i]; };
  auto slotOf = [&](int i) { return widthOf(i) + (f[i] > 0.01f ? 28 + textW * f[i] : 0.f) + gap; };
  std::vector<float> xs(n);
  float acc = 0;
  for (int i = 0; i < n; ++i) { xs[i] = acc; acc += slotOf(i); }
  // Keep up to two tiles to the left of the selection.
  float target = xs[std::max(0, sel_ - 2)];
  float maxCam = std::max(0.f, acc - gap - (W - kHome * 2));
  target = std::min(target, maxCam);
  cam_ += (target - cam_) * (1 - std::exp(-dt * 11));

  for (int i = 0; i < n; ++i) {
    float x = kHome + xs[i] - cam_;
    float s = widthOf(i);
    if (x + slotOf(i) < -20 || x > W + 20) continue;
    const GameEntry& g = games_[view_[i]];
    drawTile(g, x, cy - s / 2, s, f[i], now, i == sel_);
    if (f[i] > 0.05f) {
      float a = f[i];
      float tx = x + s + 28;
      TextStyle nm = head(28, theme::text.alpha(a), 0);
      nm.wrap = textW;
      nm.lineHeight = 1.1f;
      Vec2 nmm = gfx_.measure(g.name, nm);
      Primary p = primaryFor(g);
      const Installed* inst = installer_->installed(g.slug);
      std::string st;
      Color sc = theme::dim;
      if (inst && g.hasBuild && inst->version != g.build.version) { st = "Update ready"; sc = theme::ember; }
      else if (p.kind == Kind::Running) { st = "Playing now"; sc = theme::ok; }
      else if (p.kind == Kind::Installing) { st = "Installing"; sc = theme::ember; }
      else if (inst || g.isWeb()) { st = "Ready to play"; sc = theme::ok; }
      else if (g.hasBuild) st = "Not installed";
      TextStyle ss = head(18, sc.alpha(a), 0);
      float th = nmm.y + 6 + gfx_.measure("A", ss).y;
      gfx_.text(g.name, tx, cy - th / 2, nm);
      gfx_.text(st, tx, cy - th / 2 + nmm.y + 6, ss);
    }
    hits_.push_back({x, cy - s / 2, s, s, i});
  }
  // Fade at the edges so tiles slip under the margins instead of being chopped.
  if (cam_ < maxCam - 1) gfx_.hgrad(W - 160, 100, 160, 280, Color(14, 11, 10, 0), Color(14, 11, 10, 200));
  if (cam_ > 1) gfx_.hgrad(0, 100, kHome, 280, Color(14, 11, 10, 230), Color(14, 11, 10, 0));
}

void App::drawHints(double now) {
  // Button prompts are gone; only the library-sync spinner remains.
  if (!loadingLibrary_) return;
  const float y = 1080 - 40 - 28;
  gfx_.arc(kHome + 12, y + 14, 9, 2.5f, static_cast<float>(now) * 6, static_cast<float>(now) * 6 + 4, theme::magma);
  gfx_.text("Syncing library", kHome + 34, y + 3, head(19, theme::dim, 0, false));
}

// ---- control center -------------------------------------------------------------------

void App::drawControlCenter(double now) {
  if (!cc_.open) return;
  const float W = gfx_.W(), H = gfx_.H();
  float k = ui::easeOut(static_cast<float>((now - cc_.openedAt) / 0.22));
  gfx_.rect(0, 0, W, H, Color(8, 6, 5, static_cast<uint8_t>(150 * k)));
  gfx_.lintOcclude();

  const float sh = 600, sy = H - sh * k;
  gfx_.rrect(0, sy, W, sh + 80, 44, Color(22, 17, 15, static_cast<uint8_t>(247 * k)));
  gfx_.rrectLine(0, sy, W, sh + 80, 44, 1.5f, theme::line.alpha(k));

  // Header.
  gfx_.text("Volcano OS", kHome, sy + 48, head(20, theme::dim.alpha(k), 0, false));
  gfx_.text("Control center", kHome, sy + 78, head(40, theme::text.alpha(k), 0));
  {
    TextStyle c = head(26, theme::text.alpha(k), 0);
    const std::string clock = clockText();
    float rx = W - kHome - gfx_.measure(clock, c).x;
    gfx_.text(clock, rx, sy + 76, c);
    TextStyle on = head(22, Color(212, 200, 191).alpha(k), 0, false);
    const bool online = ccNetLoaded_ ? ccNet_.online : !offline_;
    std::string label = online ? "Online" : "Offline";
    rx -= 30 + gfx_.measure(label, on).x;
    gfx_.text(label, rx, sy + 80, on);
    rx -= 36;
    ui::icon(gfx_, ui::Icon::Wifi, rx + 12, sy + 92, 26, (online ? theme::ok : theme::warn).alpha(k));
  }

  // Status cards: [profile], network, controller.
  const bool haveTag = !player_.gamertag.empty();
  const int nCards = haveTag ? 3 : 2;
  const float cy0 = sy + 158, ch = 128, gap = 20;
  const float cw = (W - kHome * 2 - gap * (nCards - 1)) / nCards;
  int ci = 0;
  auto cardAt = [&](float x, bool focus) {
    ui::glass(gfx_, x, cy0, cw, ch, 28, k, 0.5f);
    if (focus) ui::focusRing(gfx_, x, cy0, cw, ch, 28, k);
  };
  auto tileIcon = [&](float x, Color tintc, ui::Icon ic, Color ink) {
    gfx_.rrect(x, cy0 + (ch - 64) / 2, 64, 64, 20, tintc.alpha(k));
    ui::icon(gfx_, ic, x + 32, cy0 + ch / 2, 32, ink.alpha(k));
  };
  const bool focusCards = cc_.row == 0;
  if (haveTag) {
    float x = kHome + ci * (cw + gap);
    cardAt(x, focusCards && cc_.card == ci);
    gfx_.circle(x + 28 + 36, cy0 + ch / 2, 36, theme::magma.alpha(k));
    gfx_.circle(x + 28 + 36, cy0 + ch / 2, 31, Color(42, 26, 18).alpha(k));
    TextStyle in = head(28, Color(255, 138, 85).alpha(k), 0);
    in.align = Align::Center;
    std::string ini(1, static_cast<char>(std::toupper(static_cast<unsigned char>(player_.gamertag[0]))));
    gfx_.text(ini, x + 64, cy0 + ch / 2 - gfx_.measure(ini, in).y / 2, in);
    TextStyle n = head(26, theme::text.alpha(k), 0);
    gfx_.text(gfx_.ellipsize(player_.gamertag, n, cw - 150), x + 28 + 72 + 20, cy0 + 34, n);
    gfx_.text("Level " + std::to_string(player_.level), x + 28 + 72 + 20, cy0 + 70, head(19, theme::dim.alpha(k), 0, false));
    ++ci;
  }
  {
    float x = kHome + ci * (cw + gap);
    cardAt(x, focusCards && cc_.card == ci);
    tileIcon(x + 28, Color(123, 224, 164, 36), ui::Icon::Wifi, theme::ok);
    std::string t1, t2;
    if (!ccNetLoaded_) t1 = "Checking network";
    else if (ccNet_.wifiEnabled && !ccNet_.ssid.empty()) { t1 = ccNet_.ssid; t2 = "Wi-Fi connected" + (ccNet_.wifiIp.empty() ? std::string() : ", " + ccNet_.wifiIp); }
    else if (ccNet_.wiredConnected) { t1 = "Ethernet"; t2 = ccNet_.wiredIp.empty() ? "Connected" : "Connected, " + ccNet_.wiredIp; }
    else { t1 = "Not connected"; t2 = "Open network settings"; }
    TextStyle n = head(26, theme::text.alpha(k), 0);
    TextStyle sub = head(19, theme::dim.alpha(k), 0, false);
    gfx_.text(gfx_.ellipsize(t1, n, cw - 150), x + 28 + 64 + 20, cy0 + 34, n);
    gfx_.text(gfx_.ellipsize(t2, sub, cw - 150), x + 28 + 64 + 20, cy0 + 70, sub);
    ++ci;
  }
  {
    float x = kHome + ci * (cw + gap);
    cardAt(x, focusCards && cc_.card == ci);
    tileIcon(x + 28, Color(120, 170, 255, 36), ui::Icon::Gamepad, Color(158, 194, 255));
    const bool pad = input_.hasPad();
    TextStyle n = head(26, theme::text.alpha(k), 0);
    TextStyle sub = head(19, theme::dim.alpha(k), 0, false);
    gfx_.text(pad ? "Controller connected" : "No controller", x + 28 + 64 + 20, cy0 + 34, n);
    gfx_.text(gfx_.ellipsize(pad ? "Bluetooth settings" : "Pair one in Bluetooth settings", sub, cw - 150), x + 28 + 64 + 20, cy0 + 70, sub);
  }

  // Quick actions.
  const int n = static_cast<int>(cc_.items.size());
  const float by = sy + 326, slot = 150;
  const float span = W - kHome * 2 - slot;
  for (int i = 0; i < n; ++i) {
    const CcItem& it = cc_.items[i];
    float cx = kHome + slot / 2 + (n > 1 ? span * i / (n - 1) : span / 2);
    float cyc = by + 50;
    const bool on = cc_.row == 1 && cc_.sel == i;
    float a = it.enabled ? 1.f : 0.4f;
    if (on) {
      gfx_.glow(cx, cyc, 110, 110, theme::magma.alpha(0.28f * k));
      gfx_.circle(cx, cyc, 58, Color(255, 255, 255, static_cast<uint8_t>(255 * k)));
      gfx_.circle(cx, cyc, 54, Color(22, 17, 15));
      gfx_.circle(cx, cyc, 50, theme::text.alpha(k));
    } else {
      gfx_.circle(cx, cyc, 50, Color(255, 244, 236, static_cast<uint8_t>(20 * k)));
      gfx_.ring(cx, cyc, 50, 1.2f, theme::line.alpha(k));
    }
    Color ink = on ? Color(22, 15, 11) : it.danger ? theme::err : theme::text;
    ui::icon(gfx_, it.icon, cx, cyc, 40, ink.alpha(k * a));
    TextStyle l = head(20, (on ? theme::text : theme::dim).alpha(k * a), 0, on);
    l.align = Align::Center;
    gfx_.text(gfx_.ellipsize(it.label, l, slot), cx, by + 114, l);
  }
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
  gfx_.lintOcclude();

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
  gfx_.rrect(px, py, pw, ph, 32, Color(22, 17, 15, static_cast<uint8_t>(248 * k)));
  gfx_.rrectLine(px, py, pw, ph, 32, 1.5f, theme::line.alpha(k));

  TextStyle t = head(30, theme::text.alpha(k), 5);
  gfx_.text(gfx_.ellipsize(sentence(menu_.title), t, pw - 80), px + 40, py + 36, t);
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
      gfx_.rrect(px + 16, ry, pw - 32, rowH - 8, 22, Color(247, 241, 236, static_cast<uint8_t>(36 * k)));
      gfx_.rrectLine(px + 16, ry, pw - 32, rowH - 8, 22, 2, (it.danger ? theme::err : theme::text).alpha(0.9f * k));
    }
    Color lc = !it.enabled ? theme::faint : it.danger ? theme::err : on ? theme::text : theme::dim;
    TextStyle l = head(24, lc.alpha(k), 3.5f);
    float ly = ry + (it.sub.empty() ? (rowH - 8 - gfx_.measure("A", l).y) / 2 : 8);
    gfx_.text(sentence(it.label), px + 44, ly, l);
    if (!it.sub.empty()) gfx_.text(gfx_.ellipsize(it.sub, mono(14, theme::faint, 1), pw - 120), px + 44, ly + 32, mono(14, (on ? theme::dim : theme::faint).alpha(k), 1));
  }
  float hy = py + ph - 52;
  float hx = px + 40;
  hx += ui::prompt(gfx_, input_.style(), Action::Accept, "Select", hx, hy, 22, k) + 36;
  ui::prompt(gfx_, input_.style(), Action::Back, "Back", hx, hy, 22, k);
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
  bool native = sess_.kind == SessionKind::Native || sess_.kind == SessionKind::App;
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

  const bool isApp = sess_.kind == SessionKind::App;
  const char* labels3[3] = {isApp ? "RESUME" : "RESUME GAME", "SWITCH TO VOLCANO OS", quitArmed_ ? "PRESS AGAIN TO STOP" : isApp ? "STOP PLAYING" : "QUIT GAME"};
  const char* labels2[2] = {"RESUME GAME", "EXIT TO VOLCANO OS"};
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
  hx += ui::prompt(g, input_.style(), Action::Accept, "Select", hx, hy, 22, k) + 34;
  ui::prompt(g, input_.style(), Action::Back, "RESUME", hx, hy, 22, k);
}

}  // namespace vos
