// Wi-Fi, Bluetooth and power control for the console image. On Linux this
// drives NetworkManager (nmcli) and BlueZ (bluetoothctl); elsewhere there is
// only a fixture backend for --demo. Every call blocks (it runs a child
// process), so callers run them on the worker pool.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vos {

struct NetOp {
  bool ok = false;
  std::string error;  // short, user-presentable
};

struct WifiNet {
  std::string ssid;
  std::string security;  // "" = open, otherwise e.g. "WPA2"
  int signal = 0;        // 0-100
  bool active = false;   // currently connected
  bool known = false;    // a saved profile exists
  bool secured() const { return !security.empty(); }
};

struct NetStatus {
  bool supported = false;      // a network manager is present at all
  bool wifiPresent = false;    // there is a Wi-Fi adapter
  bool wifiEnabled = false;
  bool wiredConnected = false;
  bool online = false;         // NetworkManager reports full connectivity
  std::string ssid;            // current Wi-Fi network, if any
  std::string wifiIp, wiredIp;
  int knownWifi = 0;           // number of saved Wi-Fi profiles
};

struct BtDevice {
  std::string mac, name, icon;
  bool paired = false, connected = false, trusted = false;
  int battery = -1;  // percent, -1 unknown
  bool isInput() const { return icon.rfind("input-", 0) == 0; }
};

struct BtStatus {
  bool supported = false;  // a controller (adapter) exists
  bool powered = false;
  std::string name;
};

class NetCtl {
 public:
  virtual ~NetCtl() = default;
  virtual const char* name() const = 0;

  // ---- Wi-Fi / network ----
  virtual NetStatus status() = 0;
  // rescan=false returns what the adapter already knows (instant).
  virtual std::vector<WifiNet> wifiList(bool rescan) = 0;
  virtual NetOp wifiEnable(bool on) = 0;
  // `password` empty for open or already-saved networks.
  virtual NetOp wifiConnect(const std::string& ssid, const std::string& password, bool hidden) = 0;
  virtual NetOp wifiDisconnect(const std::string& ssid) = 0;
  virtual NetOp wifiForget(const std::string& ssid) = 0;

  // ---- Bluetooth ----
  virtual BtStatus btStatus() = 0;
  virtual NetOp btPower(bool on) = 0;
  // Blocks for `seconds` while the adapter discovers.
  virtual void btScan(int seconds) = 0;
  virtual std::vector<BtDevice> btDevices() = 0;
  virtual NetOp btPair(const std::string& mac) = 0;      // pair + trust + connect
  virtual NetOp btConnect(const std::string& mac) = 0;
  virtual NetOp btDisconnect(const std::string& mac) = 0;
  virtual NetOp btRemove(const std::string& mac) = 0;

  // ---- power ----
  virtual bool canPower() const = 0;
  virtual NetOp power(bool reboot) = 0;
};

std::unique_ptr<NetCtl> createNetCtl(bool demo);
// Abort any child process still running and make further calls fail fast; used at shutdown so
// quitting never waits on a Bluetooth scan or a stuck connect.
void netctlShutdown();

// Exposed for tests: nmcli's `-t` output escapes ':' and '\' with a backslash.
std::vector<std::string> splitTerse(const std::string& line);

}  // namespace vos
