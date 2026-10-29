// Native system settings: Wi-Fi, Bluetooth and About/power. Drawn with SDL like
// the Zonto pages, driven entirely by a controller, and backed by NetCtl.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gfx.hpp"
#include "input.hpp"
#include "netctl.hpp"
#include "util.hpp"

namespace vos {

enum class SysTab { Wifi = 0, Bluetooth = 1, About = 2 };

class SystemPages {
 public:
  struct Hooks {
    std::function<void(const std::string&, Color)> toast;
    std::function<void(const std::string& title, const std::string& initial, size_t maxLen,
                       std::function<void(const std::string&)> done)> text;
    std::function<void(const std::string& title, const std::string& msg, const std::string& okLabel,
                       std::function<void()> onOk)> confirm;
    std::function<void()> tick;
    std::function<int()> padCount;  // controllers SDL can currently see
  };

  SystemPages(Gfx& g, NetCtl& net, Pool& pool, MainQueue& main, Hooks hooks)
      : g_(g), net_(net), pool_(pool), main_(main), h_(std::move(hooks)) {}

  static constexpr int kTabs = 3;
  static const char* label(int i);

  void open(SysTab t);
  SysTab tab() const { return tab_; }
  // Returns true when the screen wants to close (Back).
  bool handle(Action a);
  void update(double now);  // background refresh; call every frame while open
  void draw(double now, PadStyle style);

 private:
  struct WRow { enum Kind { Radio, Net, Hidden } kind; int idx; };
  struct BRow { enum Kind { Radio, Header, Dev } kind; int idx; };

  // ---- Wi-Fi ----
  void refreshWifi(bool rescan);
  void applyWifi(const NetStatus& s, const std::vector<WifiNet>* nets);
  std::string wifiKey() const;
  void rebuildWifiRows(const std::string& key);
  bool handleWifi(Action a);
  void drawWifi(double now, PadStyle st);
  void connectWifi(const WifiNet& n);
  void connectHidden();
  void runConnect(const std::string& ssid, const std::string& pw, bool hidden);

  // ---- Bluetooth ----
  void refreshBt(bool scan);
  void startBtScan();
  std::string btKey() const;
  void rebuildBtRows(const std::string& key);
  bool handleBt(Action a);
  void drawBt(double now, PadStyle st);

  // ---- About ----
  void refreshStatus();
  bool handleAbout(Action a);
  void drawAbout(double now, PadStyle st);

  void hints(PadStyle st, const std::vector<std::pair<Action, std::string>>& items);
  void drawBusy(float x, float y, float w, const std::string& text, double now);
  void ok(const std::string& s) { h_.toast(s, theme::ok); }
  void bad(const std::string& s) { h_.toast(s, theme::err); }

  Gfx& g_;
  NetCtl& net_;
  Pool& pool_;
  MainQueue& main_;
  Hooks h_;
  SysTab tab_ = SysTab::Wifi;

  // Wi-Fi state
  NetStatus st_;
  bool stLoaded_ = false;
  std::vector<WifiNet> nets_;
  std::vector<WRow> wrows_;
  int wSel_ = 0;
  bool wifiPoll_ = false, wifiScanning_ = false;
  double lastWifiPoll_ = 0, lastWifiScan_ = 0;
  std::string busy_;  // non-empty while a connect/pair/toggle is running

  // Bluetooth state
  BtStatus bt_;
  bool btLoaded_ = false;
  std::vector<BtDevice> devs_;
  std::vector<BRow> brows_;
  int bSel_ = 0;
  bool btPoll_ = false, btScanning_ = false;
  double lastBtPoll_ = 0, lastBtScan_ = 0;

  // About state
  int aSel_ = 0;
  double lastStatus_ = 0;
};

}  // namespace vos
