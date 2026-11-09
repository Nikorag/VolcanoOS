#include "system_pages.hpp"

#include <algorithm>
#include <cmath>

#include "widgets.hpp"

namespace vos {

namespace {

// Same layout grid and look as pages.cpp.
constexpr float kHomeM = 72.f;
constexpr float kMainX = 532.f;
constexpr float kMainTop = 140.f;
constexpr float kMainBottom = 970.f;

TextStyle head(float size, Color c, float tracking = 0, bool bold = true) {
  TextStyle t;
  t.font = Font::Head;
  t.size = size;
  t.color = c;
  t.tracking = tracking * 0.3f;  // Figtree reads best nearly unspaced
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

int bars(int signal) { return signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : signal > 0 ? 1 : 0; }

std::string kindOf(const BtDevice& d) {
  const std::string& i = d.icon;
  if (i == "input-gaming") return "Controller";
  if (i == "input-keyboard") return "Keyboard";
  if (i == "input-mouse") return "Mouse";
  if (i.rfind("audio", 0) == 0) return "Audio";
  if (i == "phone") return "Phone";
  if (i == "computer") return "Computer";
  return "Device";
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
  static const char* l[kTabs] = {"Wi-Fi", "Bluetooth", "System"};
  return l[i];
}

void SystemPages::open(SysTab t) {
  tab_ = t;
  railFocus_ = false;
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
  if (railFocus_) {
    if (a == Action::Up || a == Action::Down) {
      int t = std::max(0, std::min(kTabs - 1, static_cast<int>(tab_) + (a == Action::Down ? 1 : -1)));
      if (t != static_cast<int>(tab_)) {
        open(static_cast<SysTab>(t));
        railFocus_ = true;
        h_.tick();
      }
    } else if (a == Action::Right || a == Action::Accept) {
      railFocus_ = false;
      h_.tick();
    }
    return false;
  }
  if (a == Action::Left && (tab_ != SysTab::About || aSel_ == 0)) {
    railFocus_ = true;
    h_.tick();
    return false;
  }
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
        if (r.ok) {
          ok("CONNECTED TO " + upper(ssid));
          if (h_.wifiConnected) h_.wifiConnected();
        } else if (wasKnown && pw.empty() && r.error == "WRONG PASSWORD") bad("WRONG PASSWORD. PRESS X TO FORGET IT, THEN TRY AGAIN");
        else bad(r.error);
        refreshWifi(false);
      });
}

void SystemPages::drawWifi(double now, PadStyle st) {
  (void)st;
  const float W = g_.W();
  const float x0 = kMainX, w = W - kHomeM - kMainX, bottom = kMainBottom;
  float y = kMainTop;
  auto card = [&](float cx, float cy, float cw, float ch, float r) {
    g_.rrect(cx, cy, cw, ch, r, Color(255, 244, 236, 13));
    g_.rrectLine(cx, cy, cw, ch, r, 1.2f, theme::line);
  };
  const bool listFocus = !railFocus_;

  // Status card.
  const bool connected = st_.wifiEnabled && !st_.ssid.empty();
  card(x0, y, w, 148, 32);
  g_.rrect(x0 + 34, y + 30, 88, 88, 26, (connected ? Color(123, 224, 164, 36) : Color(255, 244, 236, 20)));
  ui::icon(g_, ui::Icon::Wifi, x0 + 78, y + 74, 44, connected ? theme::ok : theme::dim);
  std::string title = !st_.supported ? "Network control isn't available here" : !st_.wifiPresent ? "No Wi-Fi adapter found" : !st_.wifiEnabled ? "Wi-Fi is off" : connected ? "Connected to " + st_.ssid : "Not connected";
  g_.text(g_.ellipsize(title, head(32, theme::text, 0), w - 640), x0 + 150, y + 36, head(32, theme::text, 0));
  g_.text(st_.online ? "Internet is online" : st_.wiredConnected ? "Using Ethernet" : "No internet connection", x0 + 150, y + 82,
          head(20, st_.online ? theme::ok : theme::warn, 0));
  {
    float rx = x0 + w - 34;
    auto kv = [&](const char* k, const std::string& v) {
      TextStyle vs = head(22, theme::text, 0);
      float vw = std::max(g_.measure(v, vs).x, g_.measure(k, head(18, theme::faint, 0, false)).x);
      rx -= vw;
      g_.text(k, rx, y + 40, head(18, theme::faint, 0, false));
      g_.text(v, rx, y + 70, vs);
      rx -= 48;
    };
    kv("Ethernet", st_.wiredConnected ? (st_.wiredIp.empty() ? "Connected" : st_.wiredIp) : "Not connected");
    kv("IP address", st_.wifiIp.empty() ? "-" : st_.wifiIp);
  }
  y += 148 + 22;

  if (!stLoaded_) {
    ui::spinner(g_, x0 + w / 2, y + 120, 18, now);
    return;
  }
  if (wrows_.empty()) {
    TextStyle t = head(26, theme::dim, 0);
    t.align = Align::Center;
    g_.text(!st_.supported ? "This build has no NetworkManager." : "Plug in a Wi-Fi dongle, or use an Ethernet cable.", x0 + w / 2, y + 100, t);
    return;
  }

  // Radio row.
  {
    bool on = listFocus && wSel_ == 0;
    g_.rrect(x0, y, w, 84, 24, Color(255, 244, 236, on ? 23 : 13));
    if (on) ui::focusRing(g_, x0, y, w, 84, 24);
    else g_.rrectLine(x0, y, w, 84, 24, 1.2f, theme::line);
    g_.text("Wi-Fi", x0 + 30, y + (84 - g_.measure("Wi-Fi", head(24, theme::text, 0)).y) / 2, head(24, theme::text, 0));
    ui::toggleSwitch(g_, x0 + w - 30 - 68, y + 23, st_.wifiEnabled);
  }
  y += 84 + 22;
  if (!st_.wifiEnabled) {
    g_.text("Turn Wi-Fi on to see networks.", x0 + 8, y + 6, head(20, theme::dim, 0, false));
    if (!busy_.empty()) drawBusy(x0 + w - 8, y, w - 300, busy_, now);
    return;
  }

  // Header.
  g_.text("Available networks", x0 + 8, y, head(20, theme::dim, 0));
  if (!busy_.empty()) drawBusy(x0 + w - 8, y + 2, w - 300, busy_, now);
  else if (wifiScanning_ || wifiPoll_) drawBusy(x0 + w - 8, y + 2, w - 300, wifiScanning_ ? "Scanning" : "Loading", now);
  y += 30 + 16;

  // List card.
  const float rowH = 76;
  const int total = static_cast<int>(wrows_.size()) - 1;  // without the radio row
  int fit = std::max(1, static_cast<int>((bottom - y) / rowH));
  int selIdx = std::max(0, wSel_ - 1);
  int first = std::max(0, std::min(selIdx - fit + 1, total - fit));
  if (selIdx < first) first = selIdx;
  int shown = std::min(fit, total - first);
  g_.rrect(x0, y, w, shown * rowH, 28, Color(255, 244, 236, 10));
  g_.rrectLine(x0, y, w, shown * rowH, 28, 1.2f, theme::line);
  for (int k = 0; k < shown; ++k) {
    int i = 1 + first + k;
    const WRow& r = wrows_[i];
    float ry = y + k * rowH;
    bool on = listFocus && wSel_ == i;
    if (on) {
      g_.rrect(x0 + 6, ry + 4, w - 12, rowH - 8, 22, Color(255, 244, 236, 26));
      g_.rrectLine(x0 + 6, ry + 4, w - 12, rowH - 8, 22, 3, Color(255, 255, 255));
    } else if (k + 1 < shown) {
      g_.rect(x0 + 24, ry + rowH - 1, w - 48, 1, theme::line.alpha(0.6f));
    }
    if (r.kind == WRow::Hidden) {
      ui::icon(g_, ui::Icon::Plus, x0 + 46, ry + rowH / 2, 26, theme::text);
      g_.text("Join a hidden network", x0 + 78, ry + (rowH - g_.measure("A", head(22, theme::text, 0)).y) / 2, head(22, theme::text, 0));
      continue;
    }
    const WifiNet& n = nets_[r.idx];
    TextStyle nm = head(22, theme::text, 0);
    g_.text(g_.ellipsize(n.ssid, nm, w - 360), x0 + 34, ry + 14, nm);
    std::string sub = n.active ? "Connected" : n.known ? "Saved" : n.secured() ? n.security : "Open network";
    g_.text(sub, x0 + 34, ry + 43, head(17, n.active ? theme::ok : theme::faint, 0));
    float bx = x0 + w - 34 - 29;  // signal bars (4 x 5px, 3px gaps), bottom-aligned
    int lvl = bars(n.signal);
    for (int b = 0; b < 4; ++b) {
      float bh = 8.f + b * 5.3f;
      g_.rrect(bx + b * 8, ry + rowH / 2 + 12 - bh, 5, bh, 2, b < lvl ? theme::text : Color(247, 241, 236, 51));
    }
    float lx = bx - 36;
    if (n.secured()) { ui::icon(g_, ui::Icon::Lock, lx, ry + rowH / 2, 22, theme::dim); lx -= 40; }
    if (on && n.known) {
      TextStyle fs = head(17, theme::text, 0);
      float fw = g_.measure("Forget", fs).x + 36;
      g_.rrect(lx - fw + 8, ry + rowH / 2 - 20, fw, 40, 20, Color(255, 244, 236, 26));
      g_.rrectLine(lx - fw + 8, ry + rowH / 2 - 20, fw, 40, 20, 1.2f, theme::line);
      g_.text("Forget", lx - fw + 8 + 18, ry + rowH / 2 - g_.measure("Forget", fs).y / 2, fs);
    }
  }
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
  (void)st;
  const float W = g_.W();
  const float x0 = kMainX, w = W - kHomeM - kMainX, bottom = kMainBottom;
  const float asideW = 400, lw = w - asideW - 24;
  const bool listFocus = !railFocus_;
  float y = kMainTop;

  if (!btLoaded_) {
    ui::spinner(g_, x0 + w / 2, y + 160, 18, now);
    return;
  }
  if (!bt_.supported) {
    TextStyle t = head(26, theme::dim, 0);
    t.align = Align::Center;
    g_.text("No Bluetooth adapter found", x0 + w / 2, y + 120, t);
    TextStyle s = head(18, theme::faint, 0, false);
    s.align = Align::Center;
    g_.text("Wired and 2.4GHz dongle controllers still work.", x0 + w / 2, y + 170, s);
    return;
  }

  // Right: pairing help.
  {
    g_.rrect(x0 + lw + 24, y, asideW, 580, 32, Color(255, 244, 236, 13));
    g_.rrectLine(x0 + lw + 24, y, asideW, 580, 32, 1.2f, theme::line);
    float ax = x0 + lw + 24 + 32, ay = y + 32;
    g_.text("Pair a controller", ax, ay, head(24, theme::text, 0));
    TextStyle p = head(18, theme::dim, 0, false);
    p.wrap = asideW - 64;
    p.lineHeight = 1.35f;
    g_.text("Put it in pairing mode and it will show up under Nearby.", ax, ay + 42, p);
    struct Tip { const char* who; const char* how; };
    const Tip tips[] = {{"PlayStation", "Hold PS and Share until the light bar flashes."},
                        {"Xbox", "Hold the pair button on top until the logo flashes."},
                        {"Switch Pro", "Hold the small sync button on top."},
                        {"Anything else", "Look for a pair or sync button in the manual."}};
    float ty = ay + 132;
    for (auto& t : tips) {
      g_.text(t.who, ax, ty, head(18, theme::text, 0));
      p.wrap = asideW - 64;
      g_.text(t.how, ax, ty + 26, p);
      ty += 98;
    }
  }

  // Left: radio row + device groups.
  const float rowRadio = 84;
  {
    bool on = listFocus && !brows_.empty() && brows_[std::min<int>(bSel_, static_cast<int>(brows_.size()) - 1)].kind == BRow::Radio;
    g_.rrect(x0, y, lw, rowRadio, 24, Color(255, 244, 236, on ? 23 : 13));
    if (on) ui::focusRing(g_, x0, y, lw, rowRadio, 24);
    else g_.rrectLine(x0, y, lw, rowRadio, 24, 1.2f, theme::line);
    g_.text("Bluetooth", x0 + 30, y + 16, head(24, theme::text, 0));
    g_.text("Visible as " + bt_.name, x0 + 30, y + 49, head(17, theme::faint, 0, false));
    ui::toggleSwitch(g_, x0 + lw - 30 - 68, y + 23, bt_.powered);
  }
  y += rowRadio + 22;
  if (!bt_.powered) {
    g_.text("Turn Bluetooth on to pair devices.", x0 + 8, y + 6, head(20, theme::dim, 0, false));
    if (!busy_.empty()) drawBusy(x0 + lw - 8, y, lw - 200, busy_, now);
    return;
  }

  // Scrollable list of the remaining rows.
  std::vector<float> hs;
  for (auto& r : brows_) hs.push_back(r.kind == BRow::Radio ? 0.f : r.kind == BRow::Header ? 44.f : 84.f);
  int first = 1;
  {
    float avail = bottom - y;
    first = windowStart(std::vector<float>(hs.begin() + 1, hs.end()), std::max(0, bSel_ - 1), avail) + 1;
    first = std::max(1, first);
  }
  float yy = y;
  for (int i = first; i < static_cast<int>(brows_.size()); ++i) {
    if (yy + hs[i] > bottom) break;
    const BRow& r = brows_[i];
    if (r.kind == BRow::Header) {
      bool paired = r.idx == 0;
      g_.text(paired ? "My devices" : "Nearby", x0 + 8, yy + 6, head(20, theme::dim, 0));
      if (!paired) {
        if (!busy_.empty()) drawBusy(x0 + lw - 8, yy + 8, lw - 300, busy_, now);
        else if (btScanning_) drawBusy(x0 + lw - 8, yy + 8, lw - 300, "Searching", now);
      }
      yy += hs[i];
      continue;
    }
    const BtDevice& d = devs_[r.idx];
    bool on = listFocus && i == bSel_;
    float rh = hs[i] - 8;
    g_.rrect(x0, yy, lw, rh, 24, Color(255, 244, 236, on ? 26 : 10));
    if (on) ui::focusRing(g_, x0, yy, lw, rh, 24);
    else g_.rrectLine(x0, yy, lw, rh, 24, 1.2f, theme::line.alpha(0.8f));
    float cy = yy + rh / 2;
    g_.rrect(x0 + 30, cy - 24, 48, 48, 14, d.connected ? Color(158, 194, 255, 36) : Color(255, 244, 236, 20));
    ui::icon(g_, d.isInput() ? ui::Icon::Gamepad : ui::Icon::Bluetooth, x0 + 54, cy, 26, d.connected ? Color(158, 194, 255) : theme::dim);
    TextStyle nm = head(22, theme::text, 0);
    g_.text(g_.ellipsize(d.name, nm, lw - 360), x0 + 98, cy - 28, nm);
    std::string sub = d.connected ? "Connected" : d.paired ? "Paired, not connected" : kindOf(d);
    g_.text(sub, x0 + 98, cy + 4, head(17, d.connected ? theme::ok : theme::faint, 0, d.connected));
    float ex = x0 + lw - 30;
    if (!d.paired) {
      TextStyle ps = head(18, theme::text, 0);
      float pw = 82;
      g_.rrect(ex - pw, cy - 24, pw, 48, 24, Color(255, 244, 236, 15));
      g_.rrectLine(ex - pw, cy - 24, pw, 48, 24, 1.2f, Color(255, 240, 230, 60));
      ps.align = Align::Center;
      g_.text("Pair", ex - pw / 2, cy - g_.measure("Pair", ps).y / 2, ps);
    } else {
      if (d.battery >= 0) {
        char b[16];
        std::snprintf(b, sizeof b, "%d%%", d.battery);
        TextStyle bs = head(18, theme::dim, 0, false);
        bs.align = Align::Right;
        g_.text(b, ex, cy - g_.measure(b, bs).y / 2, bs);
        ui::progressBar(g_, ex - 64 - 90, cy - 3, 90, 6, d.battery / 100.f, Color(158, 194, 255), Color(247, 241, 236, 40));
        ex -= 64 + 90 + 20;
      }
      if (on) {
        TextStyle fs = head(17, theme::text, 0);
        float fw = g_.measure("Forget", fs).x + 36;
        g_.rrect(ex - fw, cy - 20, fw, 40, 20, Color(255, 244, 236, 26));
        g_.rrectLine(ex - fw, cy - 20, fw, 40, 20, 1.2f, theme::line);
        g_.text("Forget", ex - fw + 18, cy - g_.measure("Forget", fs).y / 2, fs);
      }
    }
    yy += hs[i];
  }
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
  if (a == Action::Left || a == Action::Right || a == Action::Up || a == Action::Down) {
    int d = (a == Action::Right || a == Action::Down) ? 1 : -1;
    int s = std::max(0, std::min(2, aSel_ + d));
    if (s != aSel_) {
      aSel_ = s;
      h_.tick();
    }
    return false;
  }
  if (a != Action::Accept) return false;
  if (aSel_ == 2) {
    if (h_.quit) h_.quit();
    return false;
  }
  if (!net_.canPower()) return false;
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
  (void)now; (void)st;
  const float W = g_.W();
  const float x0 = kMainX, w = W - kHomeM - kMainX;
  float y = kMainTop;
  g_.rrect(x0, y, w, 166, 32, Color(255, 244, 236, 13));
  g_.rrectLine(x0, y, w, 166, 32, 1.2f, theme::line);
  float lw = g_.logo(x0 + 34 + 48, y + 83, 80);
  float tx = x0 + 34 + std::max(lw, 96.f) + 28;
  g_.text("Volcano OS", tx, y + 38, head(36, theme::text, 0));
  g_.text("Version " VOS_VERSION, tx, y + 92, head(20, theme::dim, 0, false));
  y += 166 + 24;

  auto cell = [&](float cx, float cy, float cw, const char* k, const std::string& v, Color c, float vs) {
    g_.rrect(cx, cy, cw, 104, 24, Color(255, 244, 236, 10));
    g_.text(k, cx + 30, cy + 20, head(18, theme::faint, 0, false));
    TextStyle t = head(vs, c, 0);
    g_.text(g_.ellipsize(v, t, cw - 60), cx + 30, cy + 54, t);
  };
  const float cw = (w - 16) / 2;
  std::string net = st_.online ? (st_.wiredConnected ? "Ethernet" : st_.ssid) : "Offline";
  std::string ip = !st_.wiredIp.empty() ? st_.wiredIp : st_.wifiIp;
  int pads = h_.padCount ? h_.padCount() : 0;
  cell(x0, y, cw, "Network", net, st_.online ? theme::ok : theme::warn, 24);
  cell(x0 + cw + 16, y, cw, "IP address", ip.empty() ? "-" : ip, theme::text, 24);
  y += 104 + 16;
  cell(x0, y, cw, "Controllers", std::to_string(pads) + (pads == 1 ? " connected" : " connected"), theme::text, 24);
  cell(x0 + cw + 16, y, cw, "Network backend", net_.name(), theme::text, 24);
  y += 104 + 16;
  cell(x0, y, w, "Data folder", dataDir().string(), theme::text, 22);
  y += 104 + 22;

  g_.text("Power", x0 + 8, y, head(20, theme::dim, 0));
  y += 38;
  const bool can = net_.canPower();
  const char* titles[3] = {"Restart", "Shut down", "Quit Volcano OS"};
  ui::Icon icons[3] = {ui::Icon::Restart, ui::Icon::Power, ui::Icon::Quit};
  const float bw = (w - 32) / 3;
  for (int i = 0; i < 3; ++i) {
    float bx = x0 + i * (bw + 16);
    bool on = !railFocus_ && aSel_ == i;
    bool usable = i == 2 || can;
    g_.rrect(bx, y, bw, 96, 28, Color(255, 244, 236, on ? 23 : 10));
    if (on) ui::focusRing(g_, bx, y, bw, 96, 28);
    else g_.rrectLine(bx, y, bw, 96, 28, 1.2f, theme::line);
    TextStyle t = head(24, (usable ? theme::text : theme::faint), 0);
    float tw = g_.measure(titles[i], t).x;
    float gx = bx + bw / 2 - (tw + 40) / 2;
    ui::icon(g_, icons[i], gx + 14, y + 48, 28, usable ? theme::text : theme::faint);
    g_.text(titles[i], gx + 40, y + 48 - g_.measure(titles[i], t).y / 2, t);
  }
  if (!can) g_.text("Restart and shut down aren't available on this system.", x0 + 8, y + 96 + 20, head(18, theme::faint, 0, false));
}

// ---- shared -------------------------------------------------------------------------------------

// Left-hand section list (Wi-Fi, Bluetooth, System).
void SystemPages::drawRail() {
  const char* names[3] = {"Wi-Fi", "Bluetooth", "System"};
  ui::Icon icons[3] = {ui::Icon::Wifi, ui::Icon::Bluetooth, ui::Icon::Expand};
  float y = kMainTop;
  for (int i = 0; i < 3; ++i) {
    bool active = static_cast<int>(tab_) == i;
    g_.rrect(kHomeM, y, 400, 76, 24, active ? theme::text : Color(0, 0, 0, 0));
    if (active && railFocus_) ui::focusRing(g_, kHomeM, y, 400, 76, 24);
    Color ink = active ? Color(22, 15, 11) : Color(212, 200, 191);
    ui::icon(g_, icons[i], kHomeM + 26 + 14, y + 38, 28, ink);
    TextStyle t = head(24, ink, 0);
    g_.text(names[i], kHomeM + 26 + 28 + 18, y + 38 - g_.measure(names[i], t).y / 2, t);
    y += 84;
  }
  g_.rect(kHomeM + 8, y + 16, 384, 1, theme::line);
  g_.text("Volcano OS " VOS_VERSION, kHomeM + 26, y + 40, head(18, theme::faint, 0, false));
}

// Spinner + status text, right-aligned to `x`.
void SystemPages::drawBusy(float x, float y, float w, const std::string& text, double now) {
  TextStyle t = head(18, theme::faint, 0, false);
  t.align = Align::Right;
  std::string s = g_.ellipsize(text, t, w);
  float tw = g_.measure(s, t).x;
  g_.text(s, x, y, t);
  ui::spinner(g_, x - tw - 20, y + 11, 8, now);
}

void SystemPages::draw(double now, PadStyle style) {
  drawRail();
  switch (tab_) {
    case SysTab::Wifi: drawWifi(now, style); break;
    case SysTab::Bluetooth: drawBt(now, style); break;
    case SysTab::About: drawAbout(now, style); break;
  }
}

}  // namespace vos
