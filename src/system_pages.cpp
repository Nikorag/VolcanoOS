#include "system_pages.hpp"

#include <algorithm>
#include <cmath>

#include "widgets.hpp"

namespace vos {

namespace {

// Same layout grid and look as pages.cpp.
constexpr float kMargin = 96.f;
constexpr float kTop = 132.f;      // below the top bar
constexpr float kBottom = 1000.f;  // above the hint bar
constexpr float kLeftW = 560.f;

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

int windowStart(const std::vector<float>& h, int sel, float avail) {
  int start = 0;
  for (;; ++start) {
    float sum = 0;
    for (int i = start; i <= sel && i < static_cast<int>(h.size()); ++i) sum += h[i];
    if (sum <= avail || start >= sel) break;
  }
  return start;
}

void panel(Gfx& g, float x, float y, float w, float h, bool focus) {
  g.rrect(x, y, w, h, 10, Color(14, 11, 10, 215));
  g.rrectLine(x, y, w, h, 10, focus ? 2.f : 1.2f, focus ? theme::magma.alpha(0.85f) : theme::line);
}

void rowBg(Gfx& g, float x, float y, float w, float h, bool sel) {
  if (!sel) return;
  g.hgrad(x, y, w, h, theme::magma.alpha(0.30f), theme::magma.alpha(0.02f));
  g.rect(x, y, 4, h, theme::magma);
}

void toggle(Gfx& g, float x, float y, bool on) {
  float w = 64, h = 32;
  g.rrect(x, y, w, h, h / 2, on ? Color(255, 106, 31, 255) : theme::panelHi);
  g.rrectLine(x, y, w, h, h / 2, 1.4f, on ? Color(255, 178, 71) : theme::line);
  g.circle(x + (on ? w - h / 2 : h / 2), y + h / 2, h / 2 - 5, on ? Color(20, 10, 6) : theme::dim);
}

int bars(int signal) { return signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : signal > 0 ? 1 : 0; }

void signalBars(Gfx& g, float x, float y, int signal, Color on) {  // x,y = bottom-left
  int n = bars(signal);
  for (int i = 0; i < 4; ++i) {
    float h = 7.f + i * 5.f;
    g.rrect(x + i * 10, y - h, 6, h, 1.5f, i < n ? on : theme::line);
  }
}

void lock(Gfx& g, float cx, float cy, Color c) {
  g.arc(cx, cy - 3, 5, 2.2f, 3.14159f, 6.28318f, c);
  g.rrect(cx - 7, cy - 3, 14, 11, 2.5f, c);
}

// Fan of three arcs over a dot; `level` 0..3 lights them from the centre out.
void wifiGlyph(Gfx& g, float cx, float cy, float r, int level, Color on, Color off) {
  float oy = cy + r * 0.55f;
  g.circle(cx, oy, r * 0.11f, level >= 0 ? on : off);
  for (int i = 0; i < 3; ++i)
    g.arc(cx, oy, r * (0.42f + 0.40f * i), r * 0.09f, -2.356f, -0.785f, i < level ? on : off);
}

void btGlyph(Gfx& g, float cx, float cy, float r, Color c) {
  float t = std::max(2.f, r * 0.11f);
  g.line(cx, cy - r, cx, cy + r, t, c);
  g.line(cx, cy - r, cx + r * 0.6f, cy - r * 0.4f, t, c);
  g.line(cx + r * 0.6f, cy - r * 0.4f, cx - r * 0.6f, cy + r * 0.45f, t, c);
  g.line(cx, cy + r, cx + r * 0.6f, cy + r * 0.4f, t, c);
  g.line(cx + r * 0.6f, cy + r * 0.4f, cx - r * 0.6f, cy - r * 0.45f, t, c);
}

std::string kindOf(const BtDevice& d) {
  const std::string& i = d.icon;
  if (i == "input-gaming") return "CONTROLLER";
  if (i == "input-keyboard") return "KEYBOARD";
  if (i == "input-mouse") return "MOUSE";
  if (i.rfind("audio", 0) == 0) return "AUDIO";
  if (i == "phone") return "PHONE";
  if (i == "computer") return "COMPUTER";
  return "DEVICE";
}

struct WifiSnap {
  NetStatus st;
  std::vector<WifiNet> nets;
};
struct BtSnap {
  BtStatus st;
  std::vector<BtDevice> devs;
};

}  // namespace

const char* SystemPages::label(int i) {
  static const char* l[kTabs] = {"WI-FI", "BLUETOOTH", "ABOUT"};
  return l[i];
}

void SystemPages::open(SysTab t) {
  tab_ = t;
  switch (t) {
    case SysTab::Wifi:
      lastWifiScan_ = 0;  // let update() kick off a rescan
      refreshWifi(false);
      break;
    case SysTab::Bluetooth:
      refreshBt(false);
      break;
    case SysTab::About:
      refreshStatus();
      break;
  }
}

bool SystemPages::handle(Action a) {
  if (a == Action::LB || a == Action::RB) {
    int t = (static_cast<int>(tab_) + (a == Action::RB ? 1 : kTabs - 1)) % kTabs;
    open(static_cast<SysTab>(t));
    h_.tick();
    return false;
  }
  if (a == Action::Back || a == Action::Home || a == Action::Start) return true;
  switch (tab_) {
    case SysTab::Wifi: return handleWifi(a);
    case SysTab::Bluetooth: return handleBt(a);
    case SysTab::About: return handleAbout(a);
  }
  return false;
}

void SystemPages::update(double now) {
  switch (tab_) {
    case SysTab::Wifi:
      if (!busy_.empty() || wifiPoll_ || wifiScanning_) break;
      if (now - lastWifiScan_ > 30) refreshWifi(true);
      else if (now - lastWifiPoll_ > 5) refreshWifi(false);
      break;
    case SysTab::Bluetooth:
      if (btScanning_) {
        if (!btPoll_ && now - lastBtPoll_ > 2.5) refreshBt(false);  // show devices as they appear
      } else if (busy_.empty()) {
        if (bt_.powered && now - lastBtScan_ > 45) startBtScan();
        else if (!btPoll_ && now - lastBtPoll_ > 6) refreshBt(false);
      }
      break;
    case SysTab::About:
      if (now - lastStatus_ > 5) refreshStatus();
      break;
  }
}

// ---- Wi-Fi ---------------------------------------------------------------------------

void SystemPages::refreshWifi(bool rescan) {
  if (wifiPoll_) return;
  wifiPoll_ = true;
  lastWifiPoll_ = nowSeconds();
  pool_.run<WifiSnap>(
      main_,
      [this] {
        WifiSnap s;
        s.st = net_.status();
        if (s.st.wifiEnabled) s.nets = net_.wifiList(false);
        return s;
      },
      [this, rescan](WifiSnap s) {
        wifiPoll_ = false;
        applyWifi(s.st, s.st.wifiEnabled ? &s.nets : nullptr);
        if (!rescan || !st_.wifiEnabled || wifiScanning_) return;
        wifiScanning_ = true;
        lastWifiScan_ = nowSeconds();
        pool_.run<std::vector<WifiNet>>(
            main_, [this] { return net_.wifiList(true); },
            [this](std::vector<WifiNet> n) {
              wifiScanning_ = false;
              applyWifi(st_, &n);
            });
      });
}

std::string SystemPages::wifiKey() const {
  if (wSel_ < 0 || wSel_ >= static_cast<int>(wrows_.size())) return {};
  const WRow& r = wrows_[wSel_];
  if (r.kind == WRow::Net) return r.idx < static_cast<int>(nets_.size()) ? nets_[r.idx].ssid : std::string();
  return r.kind == WRow::Radio ? "\1radio" : "\1hidden";
}

void SystemPages::applyWifi(const NetStatus& s, const std::vector<WifiNet>* nets) {
  const std::string key = wifiKey();  // before nets_ changes under the row indices
  st_ = s;
  stLoaded_ = true;
  if (nets) nets_ = *nets;
  else if (!s.wifiEnabled) nets_.clear();
  rebuildWifiRows(key);
}

void SystemPages::rebuildWifiRows(const std::string& key) {
  wrows_.clear();
  if (st_.wifiPresent) {
    wrows_.push_back({WRow::Radio, 0});
    if (st_.wifiEnabled) {
      for (int i = 0; i < static_cast<int>(nets_.size()); ++i) wrows_.push_back({WRow::Net, i});
      wrows_.push_back({WRow::Hidden, 0});
    }
  }
  int n = static_cast<int>(wrows_.size());
  for (int i = 0; i < n; ++i) {
    const WRow& r = wrows_[i];
    std::string k = r.kind == WRow::Net ? nets_[r.idx].ssid : r.kind == WRow::Radio ? "\1radio" : "\1hidden";
    if (k == key) {
      wSel_ = i;
      return;
    }
  }
  wSel_ = std::max(0, std::min(wSel_, n - 1));
}

bool SystemPages::handleWifi(Action a) {
  int n = static_cast<int>(wrows_.size());
  if (a == Action::Up || a == Action::Down) {
    int s = wSel_ + (a == Action::Down ? 1 : -1);
    if (s >= 0 && s < n) {
      wSel_ = s;
      h_.tick();
    }
    return false;
  }
  if (a == Action::Y) {
    lastWifiScan_ = 0;
    refreshWifi(true);
    return false;
  }
  if (n == 0 || wSel_ >= n) return false;
  const WRow row = wrows_[wSel_];
  if (a == Action::Accept) {
    if (!busy_.empty()) return false;
    if (row.kind == WRow::Radio) {
      bool on = !st_.wifiEnabled;
      busy_ = on ? "TURNING WI-FI ON" : "TURNING WI-FI OFF";
      pool_.run<NetOp>(
          main_, [this, on] { return net_.wifiEnable(on); },
          [this, on](NetOp r) {
            busy_.clear();
            if (!r.ok) bad(r.error);
            refreshWifi(on);
          });
    } else if (row.kind == WRow::Net) {
      connectWifi(nets_[row.idx]);
    } else {
      connectHidden();
    }
  } else if (a == Action::X && row.kind == WRow::Net && nets_[row.idx].known && busy_.empty()) {
    WifiNet n0 = nets_[row.idx];
    h_.confirm("FORGET " + upper(n0.ssid) + "?", "Removes the saved password. You'll need to enter it again to rejoin.", "FORGET NETWORK",
               [this, n0] {
                 busy_ = "FORGETTING " + upper(n0.ssid);
                 pool_.run<NetOp>(
                     main_, [this, n0] { return net_.wifiForget(n0.ssid); },
                     [this, n0](NetOp r) {
                       busy_.clear();
                       if (!r.ok) bad(r.error);
                       else h_.toast("FORGOT " + upper(n0.ssid), theme::dim);
                       refreshWifi(false);
                     });
               });
  }
  return false;
}

void SystemPages::connectWifi(const WifiNet& n) {
  if (n.active) return h_.toast("ALREADY CONNECTED TO " + upper(n.ssid), theme::dim);
  if (n.known || !n.secured()) return runConnect(n.ssid, "", false);
  std::string ssid = n.ssid;
  h_.text("PASSWORD FOR " + upper(ssid), "", 63, [this, ssid](const std::string& pw) {
    if (pw.empty()) return;
    if (pw.size() < 8) return bad("PASSWORD MUST BE AT LEAST 8 CHARACTERS");
    runConnect(ssid, pw, false);
  });
}

void SystemPages::connectHidden() {
  h_.text("NETWORK NAME", "", 32, [this](const std::string& ssid) {
    if (ssid.empty()) return;
    h_.text("PASSWORD FOR " + upper(ssid) + " (BLANK IF OPEN)", "", 63, [this, ssid](const std::string& pw) {
      if (!pw.empty() && pw.size() < 8) return bad("PASSWORD MUST BE AT LEAST 8 CHARACTERS");
      runConnect(ssid, pw, true);
    });
  });
}

void SystemPages::runConnect(const std::string& ssid, const std::string& pw, bool hidden) {
  bool wasKnown = std::any_of(nets_.begin(), nets_.end(), [&](const WifiNet& n) { return n.ssid == ssid && n.known; });
  busy_ = "CONNECTING TO " + upper(ssid);
  pool_.run<NetOp>(
      main_, [this, ssid, pw, hidden] { return net_.wifiConnect(ssid, pw, hidden); },
      [this, ssid, pw, wasKnown](NetOp r) {
        busy_.clear();
        if (r.ok) ok("CONNECTED TO " + upper(ssid));
        else if (wasKnown && pw.empty() && r.error == "WRONG PASSWORD") bad("WRONG PASSWORD. PRESS X TO FORGET IT, THEN TRY AGAIN");
        else bad(r.error);
        refreshWifi(false);
      });
}

void SystemPages::drawWifi(double now, PadStyle st) {
  const float W = g_.W();
  float top = kTop + 8, bottom = kBottom, rx = kMargin + kLeftW + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, kLeftW, bottom - top, false);
  panel(g_, rx, top, rw, bottom - top, true);

  // ---- status card ----
  bool connected = st_.wifiEnabled && !st_.ssid.empty();
  float cx = kMargin + 100, cy = top + 120;
  g_.glow(cx, cy, 130, 130, (connected ? theme::ok : theme::magma).alpha(0.18f));
  g_.circle(cx, cy, 62, theme::panelHi);
  g_.ring(cx, cy, 62, 3, connected ? theme::ok : theme::line);
  int lvl = 0;
  for (auto& n : nets_)
    if (n.active) lvl = std::max(1, bars(n.signal) * 3 / 4);
  wifiGlyph(g_, cx, cy - 6, 40, connected ? std::max(lvl, 1) : -1, connected ? theme::ok : theme::dim, theme::line);
  float tx = cx + 92;
  std::string state = !st_.supported ? "NOT AVAILABLE" : !st_.wifiPresent ? "NO ADAPTER" : !st_.wifiEnabled ? "WI-FI OFF" : connected ? "CONNECTED" : "NOT CONNECTED";
  g_.text(state, tx, top + 70, head(30, theme::text, 3));
  g_.text(g_.ellipsize(connected ? upper(st_.ssid) : st_.wiredConnected ? "USING ETHERNET" : "", mono(16, theme::ember, 3), kLeftW - 230), tx, top + 118, mono(16, theme::ember, 3));

  auto kv = [&](float y, const char* k, const std::string& v, Color c) {
    g_.text(k, kMargin + 36, y, mono(13, theme::faint, 4));
    g_.text(g_.ellipsize(v, head(24, c, 1), kLeftW - 72), kMargin + 36, y + 22, head(24, c, 1));
  };
  kv(top + 230, "INTERNET", st_.online ? "ONLINE" : "OFFLINE", st_.online ? theme::ok : theme::warn);
  kv(top + 310, "WI-FI ADDRESS", st_.wifiIp.empty() ? "-" : st_.wifiIp, theme::text);
  kv(top + 390, "ETHERNET", st_.wiredConnected ? (st_.wiredIp.empty() ? "CONNECTED" : st_.wiredIp) : "NOT CONNECTED", st_.wiredConnected ? theme::text : theme::dim);
  TextStyle tip = head(18, theme::faint, 0, false);
  tip.wrap = kLeftW - 72;
  g_.text("Pick a network and press A to join. Passwords are typed with the on-screen keyboard. Wired connections work automatically.", kMargin + 36, bottom - 150, tip);

  // ---- network list ----
  g_.text("NETWORKS", rx + 40, top + 24, mono(13, theme::faint, 4));
  if (!busy_.empty()) drawBusy(rx + rw - 40, top + 22, rw - 260, busy_, now);
  else if (wifiScanning_ || wifiPoll_) drawBusy(rx + rw - 40, top + 22, rw - 260, wifiScanning_ ? "SCANNING" : "LOADING", now);
  g_.rect(rx + 30, top + 56, rw - 60, 1, theme::line);

  float lt = top + 68;
  if (!stLoaded_) return hints(st, {});
  if (wrows_.empty()) {
    TextStyle t = head(26, theme::dim, 4);
    t.align = Align::Center;
    g_.text(!st_.supported ? "NETWORK CONTROL ISN'T AVAILABLE HERE" : "NO WI-FI ADAPTER FOUND", rx + rw / 2, top + 300, t);
    TextStyle s = mono(15, theme::faint, 1);
    s.align = Align::Center;
    g_.text(!st_.supported ? "This build has no NetworkManager." : "Plug in a Wi-Fi dongle, or use an Ethernet cable.", rx + rw / 2, top + 350, s);
    return hints(st, {});
  }
  std::vector<float> hs(wrows_.size(), 80.f);
  int first = windowStart(hs, wSel_, bottom - lt - 12);
  float y = lt;
  for (int i = first; i < static_cast<int>(wrows_.size()); ++i) {
    if (y + 80 > bottom - 6) break;
    const WRow& r = wrows_[i];
    bool on = i == wSel_;
    rowBg(g_, rx + 10, y, rw - 20, 74, on);
    TextStyle nm = head(21, on ? theme::text : theme::dim, 3);
    if (r.kind == WRow::Radio) {
      g_.text("WI-FI", rx + 40, y + 12, nm);
      g_.text(st_.wifiEnabled ? "Radio is on" : "Radio is off. Turn it on to see networks.", rx + 40, y + 44, mono(13, theme::faint, 1));
      toggle(g_, rx + rw - 40 - 64, y + 20, st_.wifiEnabled);
    } else if (r.kind == WRow::Hidden) {
      g_.text("OTHER NETWORK...", rx + 40, y + 12, nm);
      g_.text("Join a hidden network by name", rx + 40, y + 44, mono(13, theme::faint, 1));
    } else {
      const WifiNet& n = nets_[r.idx];
      g_.text(g_.ellipsize(upper(n.ssid), nm, rw - 330), rx + 40, y + 12, nm);
      std::string sub = n.active ? "CONNECTED" : n.known ? "SAVED" : n.secured() ? n.security : "OPEN";
      if (n.active && !st_.wifiIp.empty()) sub += "  ·  " + st_.wifiIp;
      g_.text(sub, rx + 40, y + 44, mono(13, n.active ? theme::ok : theme::faint, 1.5f));
      float bx = rx + rw - 40 - 36;
      signalBars(g_, bx, y + 50, n.signal, n.active ? theme::ok : theme::text);
      if (n.secured()) lock(g_, bx - 30, y + 38, theme::dim);
      if (on && n.known) {
        float pw0 = ui::promptWidth(g_, st, Action::X, "FORGET", 20);
        ui::prompt(g_, st, Action::X, "FORGET", bx - 70 - pw0, y + 14, 20);
      }
    }
    y += 80;
  }
  std::vector<std::pair<Action, std::string>> hs2;
  const WRow& cur = wrows_[std::min<int>(wSel_, static_cast<int>(wrows_.size()) - 1)];
  hs2.push_back({Action::Accept, cur.kind == WRow::Radio ? "TOGGLE" : cur.kind == WRow::Hidden ? "ENTER NAME" : "CONNECT"});
  hs2.push_back({Action::Y, "RESCAN"});
  hints(st, hs2);
}

// ---- Bluetooth -------------------------------------------------------------------------

void SystemPages::refreshBt(bool scan) {
  if (btPoll_) return;
  btPoll_ = true;
  lastBtPoll_ = nowSeconds();
  pool_.run<BtSnap>(
      main_,
      [this] {
        BtSnap s;
        s.st = net_.btStatus();
        if (s.st.powered) s.devs = net_.btDevices();
        return s;
      },
      [this, scan](BtSnap s) {
        btPoll_ = false;
        const std::string key = btKey();  // before devs_ changes under the row indices
        bt_ = s.st;
        btLoaded_ = true;
        devs_ = bt_.powered ? std::move(s.devs) : std::vector<BtDevice>{};
        rebuildBtRows(key);
        if (scan && bt_.powered) startBtScan();
      });
}

void SystemPages::startBtScan() {
  if (btScanning_ || !bt_.powered) return;
  btScanning_ = true;
  lastBtScan_ = nowSeconds();
  pool_.run<int>(
      main_, [this] { net_.btScan(10); return 0; },
      [this](int) {
        btScanning_ = false;
        refreshBt(false);
      });
}

std::string SystemPages::btKey() const {
  if (bSel_ < 0 || bSel_ >= static_cast<int>(brows_.size())) return {};
  const BRow& r = brows_[bSel_];
  if (r.kind == BRow::Dev) return r.idx < static_cast<int>(devs_.size()) ? devs_[r.idx].mac : std::string();
  return "\1radio";
}

void SystemPages::rebuildBtRows(const std::string& key) {
  brows_.clear();
  if (bt_.supported) {
    brows_.push_back({BRow::Radio, 0});
    if (bt_.powered) {
      bool anyPaired = std::any_of(devs_.begin(), devs_.end(), [](const BtDevice& d) { return d.paired; });
      bool anyNear = std::any_of(devs_.begin(), devs_.end(), [](const BtDevice& d) { return !d.paired; });
      if (anyPaired) brows_.push_back({BRow::Header, 0});
      for (int i = 0; i < static_cast<int>(devs_.size()); ++i)
        if (devs_[i].paired) brows_.push_back({BRow::Dev, i});
      if (anyNear) brows_.push_back({BRow::Header, 1});
      for (int i = 0; i < static_cast<int>(devs_.size()); ++i)
        if (!devs_[i].paired) brows_.push_back({BRow::Dev, i});
    }
  }
  int n = static_cast<int>(brows_.size());
  for (int i = 0; i < n; ++i) {
    const BRow& r = brows_[i];
    if (r.kind == BRow::Header) continue;
    std::string k = r.kind == BRow::Dev ? devs_[r.idx].mac : "\1radio";
    if (k == key) {
      bSel_ = i;
      return;
    }
  }
  bSel_ = std::max(0, std::min(bSel_, n - 1));
  while (bSel_ < n && brows_[bSel_].kind == BRow::Header) ++bSel_;
  bSel_ = std::min(bSel_, std::max(0, n - 1));
}

bool SystemPages::handleBt(Action a) {
  int n = static_cast<int>(brows_.size());
  if (a == Action::Up || a == Action::Down) {
    int d = a == Action::Down ? 1 : -1;
    int s = bSel_;
    do { s += d; } while (s >= 0 && s < n && brows_[s].kind == BRow::Header);
    if (s >= 0 && s < n) {
      bSel_ = s;
      h_.tick();
    }
    return false;
  }
  if (a == Action::Y) {
    if (bt_.powered) startBtScan();
    return false;
  }
  if (n == 0 || bSel_ >= n) return false;
  const BRow row = brows_[bSel_];
  if (a == Action::Accept) {
    if (!busy_.empty()) return false;
    if (row.kind == BRow::Radio) {
      bool on = !bt_.powered;
      busy_ = on ? "TURNING BLUETOOTH ON" : "TURNING BLUETOOTH OFF";
      pool_.run<NetOp>(
          main_, [this, on] { return net_.btPower(on); },
          [this, on](NetOp r) {
            busy_.clear();
            if (!r.ok) bad(r.error);
            lastBtScan_ = 0;
            refreshBt(on);
          });
    } else if (row.kind == BRow::Dev) {
      BtDevice d = devs_[row.idx];
      std::string nm = upper(d.name);
      const bool disconnecting = d.connected, pairing = !d.paired;
      busy_ = disconnecting ? "DISCONNECTING " + nm : pairing ? "PAIRING WITH " + nm : "CONNECTING TO " + nm;
      pool_.run<NetOp>(
          main_,
          [this, d, disconnecting, pairing] {
            return disconnecting ? net_.btDisconnect(d.mac) : pairing ? net_.btPair(d.mac) : net_.btConnect(d.mac);
          },
          [this, nm, disconnecting, pairing](NetOp r) {
            busy_.clear();
            if (!r.ok) bad(r.error);
            else if (disconnecting) h_.toast("DISCONNECTED " + nm, theme::dim);
            else ok((pairing ? "PAIRED WITH " : "CONNECTED TO ") + nm);
            refreshBt(false);
          });
    }
  } else if (a == Action::X && row.kind == BRow::Dev && devs_[row.idx].paired && busy_.empty()) {
    BtDevice d = devs_[row.idx];
    h_.confirm("FORGET " + upper(d.name) + "?", "Removes the pairing. Put it back in pairing mode to use it again.", "FORGET DEVICE", [this, d] {
      busy_ = "REMOVING " + upper(d.name);
      pool_.run<NetOp>(
          main_, [this, d] { return net_.btRemove(d.mac); },
          [this, d](NetOp r) {
            busy_.clear();
            if (!r.ok) bad(r.error);
            else h_.toast("FORGOT " + upper(d.name), theme::dim);
            refreshBt(false);
          });
    });
  }
  return false;
}

void SystemPages::drawBt(double now, PadStyle st) {
  const float W = g_.W();
  float top = kTop + 8, bottom = kBottom, rx = kMargin + kLeftW + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, kLeftW, bottom - top, false);
  panel(g_, rx, top, rw, bottom - top, true);

  int pads = h_.padCount ? h_.padCount() : 0;
  float cx = kMargin + 100, cy = top + 120;
  Color accent = bt_.powered ? Color(90, 160, 255) : theme::dim;
  g_.glow(cx, cy, 130, 130, accent.alpha(bt_.powered ? 0.2f : 0.08f));
  g_.circle(cx, cy, 62, theme::panelHi);
  g_.ring(cx, cy, 62, 3, bt_.powered ? accent : theme::line);
  btGlyph(g_, cx, cy, 30, accent);
  float tx = cx + 92;
  std::string state = !btLoaded_ ? "LOADING" : !bt_.supported ? "NO ADAPTER" : bt_.powered ? "BLUETOOTH ON" : "BLUETOOTH OFF";
  g_.text(state, tx, top + 70, head(30, theme::text, 3));
  if (bt_.supported) g_.text(g_.ellipsize(upper(bt_.name), mono(16, theme::ember, 3), kLeftW - 230), tx, top + 118, mono(16, theme::ember, 3));

  g_.text("CONTROLLERS CONNECTED", kMargin + 36, top + 230, mono(13, theme::faint, 4));
  g_.text(std::to_string(pads), kMargin + 36, top + 252, head(40, pads ? theme::ok : theme::dim, 1));

  g_.text("PAIRING A CONTROLLER", kMargin + 36, top + 350, mono(13, theme::faint, 4));
  struct Tip { const char* who; const char* how; };
  const Tip tips[] = {{"PLAYSTATION", "Hold PS + SHARE until the light bar flashes."},
                      {"XBOX", "Hold the pair button on top until the logo flashes."},
                      {"SWITCH PRO", "Hold the small sync button on top."},
                      {"OTHERS", "Look for a pair or sync button in the manual."}};
  float y = top + 388;
  for (auto& t : tips) {
    g_.text(t.who, kMargin + 36, y, head(18, theme::text, 2));
    TextStyle how = head(17, theme::faint, 0, false);
    how.wrap = kLeftW - 72;
    g_.text(t.how, kMargin + 36, y + 26, how);
    y += 84;
  }

  g_.text("DEVICES", rx + 40, top + 24, mono(13, theme::faint, 4));
  if (!busy_.empty()) drawBusy(rx + rw - 40, top + 22, rw - 260, busy_, now);
  else if (btScanning_) drawBusy(rx + rw - 40, top + 22, rw - 260, "SEARCHING", now);
  g_.rect(rx + 30, top + 56, rw - 60, 1, theme::line);

  float lt = top + 68;
  if (!btLoaded_) return hints(st, {});
  if (brows_.empty()) {
    TextStyle t = head(26, theme::dim, 4);
    t.align = Align::Center;
    g_.text("NO BLUETOOTH ADAPTER FOUND", rx + rw / 2, top + 300, t);
    TextStyle s = mono(15, theme::faint, 1);
    s.align = Align::Center;
    g_.text("Wired and 2.4GHz dongle controllers still work.", rx + rw / 2, top + 350, s);
    return hints(st, {});
  }
  std::vector<float> hs;
  for (auto& r : brows_) hs.push_back(r.kind == BRow::Header ? 50.f : 80.f);
  int first = windowStart(hs, bSel_, bottom - lt - 12);
  float yy = lt;
  for (int i = first; i < static_cast<int>(brows_.size()); ++i) {
    if (yy + hs[i] > bottom - 6) break;
    const BRow& r = brows_[i];
    bool on = i == bSel_;
    if (r.kind == BRow::Header) {
      g_.text(r.idx == 0 ? "MY DEVICES" : "NEARBY", rx + 40, yy + 22, mono(13, theme::faint, 4));
    } else {
      rowBg(g_, rx + 10, yy, rw - 20, 74, on);
      TextStyle nm = head(21, on ? theme::text : theme::dim, 3);
      if (r.kind == BRow::Radio) {
        g_.text("BLUETOOTH", rx + 40, yy + 12, nm);
        g_.text(bt_.powered ? "Radio is on" : "Radio is off. Turn it on to pair devices.", rx + 40, yy + 44, mono(13, theme::faint, 1));
        toggle(g_, rx + rw - 40 - 64, yy + 20, bt_.powered);
      } else {
        const BtDevice& d = devs_[r.idx];
        g_.text(g_.ellipsize(upper(d.name), nm, rw - 330), rx + 40, yy + 12, nm);
        std::string sub = kindOf(d) + (d.connected ? "  ·  CONNECTED" : d.paired ? "  ·  PAIRED" : "  ·  PRESS A TO PAIR");
        g_.text(sub, rx + 40, yy + 44, mono(13, d.connected ? theme::ok : theme::faint, 1.5f));
        float ex = rx + rw - 40;
        if (d.battery >= 0) {
          char b[16];
          std::snprintf(b, sizeof b, "%d%%", d.battery);
          TextStyle bs = mono(15, d.battery < 20 ? theme::warn : theme::dim, 1);
          bs.align = Align::Right;
          g_.text(b, ex, yy + 26, bs);
          ex -= 70;
        }
        if (d.connected) g_.circle(rx + rw - 52 - (d.battery >= 0 ? 70 : 0), yy + 36, 5, theme::ok);
        if (on && d.paired) {
          float pw0 = ui::promptWidth(g_, st, Action::X, "FORGET", 20);
          ui::prompt(g_, st, Action::X, "FORGET", ex - 40 - pw0, yy + 14, 20);
        }
      }
    }
    yy += hs[i];
  }
  std::vector<std::pair<Action, std::string>> hs2;
  const BRow& cur = brows_[std::min<int>(bSel_, static_cast<int>(brows_.size()) - 1)];
  if (cur.kind == BRow::Radio) hs2.push_back({Action::Accept, "TOGGLE"});
  else if (cur.kind == BRow::Dev) hs2.push_back({Action::Accept, devs_[cur.idx].connected ? "DISCONNECT" : devs_[cur.idx].paired ? "CONNECT" : "PAIR"});
  if (bt_.powered) hs2.push_back({Action::Y, "SEARCH AGAIN"});
  hints(st, hs2);
}

// ---- About / power ------------------------------------------------------------------------

void SystemPages::refreshStatus() {
  lastStatus_ = nowSeconds();
  pool_.run<NetStatus>(
      main_, [this] { return net_.status(); },
      [this](NetStatus s) {
        st_ = s;
        stLoaded_ = true;
      });
}

bool SystemPages::handleAbout(Action a) {
  if (a == Action::Up || a == Action::Down) {
    int s = aSel_ + (a == Action::Down ? 1 : -1);
    if (s >= 0 && s < 2) {
      aSel_ = s;
      h_.tick();
    }
    return false;
  }
  if (a != Action::Accept || !net_.canPower()) return false;
  const bool reboot = aSel_ == 0;
  h_.confirm(reboot ? "RESTART THE CONSOLE?" : "SHUT DOWN THE CONSOLE?",
             reboot ? "Any running game will be closed." : "Wait for the light to stop blinking before you unplug it.",
             reboot ? "RESTART" : "SHUT DOWN", [this, reboot] {
               pool_.run<NetOp>(
                   main_, [this, reboot] { return net_.power(reboot); },
                   [this](NetOp r) {
                     if (!r.ok) bad(r.error);
                   });
             });
  return false;
}

void SystemPages::drawAbout(double now, PadStyle st) {
  (void)now;
  const float W = g_.W();
  float top = kTop + 8, bottom = kBottom, rx = kMargin + kLeftW + 36, rw = W - kMargin - rx;
  panel(g_, kMargin, top, kLeftW, bottom - top, false);
  panel(g_, rx, top, rw, bottom - top, true);

  g_.text("VOLCANO OS", kMargin + 36, top + 40, head(38, theme::text, 4));
  g_.text("VERSION " VOS_VERSION, kMargin + 36, top + 92, mono(16, theme::ember, 4));
  auto kv = [&](float y, const char* k, const std::string& v, Color c) {
    g_.text(k, kMargin + 36, y, mono(13, theme::faint, 4));
    g_.text(g_.ellipsize(v, head(22, c, 1), kLeftW - 72), kMargin + 36, y + 22, head(22, c, 1));
  };
  std::string net = st_.online ? (st_.wiredConnected ? "ETHERNET" : upper(st_.ssid)) : "OFFLINE";
  std::string ip = !st_.wiredIp.empty() ? st_.wiredIp : st_.wifiIp;
  kv(top + 170, "NETWORK", net, st_.online ? theme::ok : theme::warn);
  kv(top + 250, "ADDRESS", ip.empty() ? "-" : ip, theme::text);
  kv(top + 330, "CONTROLLERS", std::to_string(h_.padCount ? h_.padCount() : 0), theme::text);
  kv(top + 410, "DATA", dataDir().string(), theme::dim);
  kv(top + 490, "NETWORK BACKEND", net_.name(), theme::dim);

  g_.text("POWER", rx + 40, top + 24, mono(13, theme::faint, 4));
  g_.rect(rx + 30, top + 56, rw - 60, 1, theme::line);
  const bool can = net_.canPower();
  const char* titles[2] = {"RESTART", "SHUT DOWN"};
  const char* subs[2] = {"Reboot the console", "Power off safely"};
  for (int i = 0; i < 2; ++i) {
    float y = top + 68 + i * 80;
    rowBg(g_, rx + 10, y, rw - 20, 74, i == aSel_);
    g_.text(titles[i], rx + 40, y + 12, head(21, i == aSel_ && can ? theme::text : theme::dim, 3));
    g_.text(can ? subs[i] : "Not available on this system", rx + 40, y + 44, mono(13, theme::faint, 1));
  }
  hints(st, can ? std::vector<std::pair<Action, std::string>>{{Action::Accept, "SELECT"}} : std::vector<std::pair<Action, std::string>>{});
}

// ---- shared -------------------------------------------------------------------------------------

void SystemPages::hints(PadStyle st, const std::vector<std::pair<Action, std::string>>& items) {
  float x = kMargin, y = 1026;
  for (auto& [a, l] : items) x += ui::prompt(g_, st, a, l, x, y, 22) + 44;
  float w = ui::promptWidth(g_, st, Action::Back, "BACK", 22);
  ui::prompt(g_, st, Action::Back, "BACK", g_.W() - kMargin - w, y, 22);
}

// Spinner + status text, right-aligned to `x`.
void SystemPages::drawBusy(float x, float y, float w, const std::string& text, double now) {
  TextStyle t = mono(13, theme::ember, 3);
  t.align = Align::Right;
  std::string s = g_.ellipsize(text, t, w);
  float tw = g_.measure(s, t).x;
  g_.text(s, x, y + 2, t);
  g_.arc(x - tw - 22, y + 10, 9, 2.5f, static_cast<float>(now) * 5, static_cast<float>(now) * 5 + 4.2f, theme::magma);
}

void SystemPages::draw(double now, PadStyle style) {
  switch (tab_) {
    case SysTab::Wifi: drawWifi(now, style); break;
    case SysTab::Bluetooth: drawBt(now, style); break;
    case SysTab::About: drawAbout(now, style); break;
  }
}

}  // namespace vos
