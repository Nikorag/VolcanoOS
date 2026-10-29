#include "netctl.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "util.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace vos {

std::vector<std::string> splitTerse(const std::string& line) {
  std::vector<std::string> out(1);
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '\\' && i + 1 < line.size()) out.back() += line[++i];
    else if (c == ':') out.emplace_back();
    else out.back() += c;
  }
  return out;
}

namespace {

std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t n = s.find('\n', p);
    std::string l = s.substr(p, n == std::string::npos ? std::string::npos : n - p);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty()) out.push_back(l);
    if (n == std::string::npos) break;
    p = n + 1;
  }
  return out;
}

bool validMac(const std::string& m) {
  if (m.size() != 17) return false;
  for (size_t i = 0; i < m.size(); ++i) {
    if (i % 3 == 2) {
      if (m[i] != ':') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(m[i]))) {
      return false;
    }
  }
  return true;
}

NetOp fail(std::string e) { return NetOp{false, std::move(e)}; }
NetOp good() { return NetOp{true, {}}; }

#ifndef _WIN32

std::atomic<bool> g_cancel{false};

struct CmdResult {
  int code = -1;  // exit status, -1 = could not run, -2 = timed out
  std::string out;
};

// Runs argv (no shell) with stdout+stderr captured. LC_ALL=C keeps the tools'
// output unlocalised so the parsing below is stable.
CmdResult runCmd(const std::vector<std::string>& args, double timeoutSec) {
  CmdResult r;
  if (g_cancel) return r;
  int pipefd[2];
  if (pipe(pipefd) != 0) return r;

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, pipefd[0]);
  posix_spawn_file_actions_addclose(&fa, pipefd[1]);

  std::vector<std::string> envStore;
  for (char** e = environ; e && *e; ++e)
    if (std::strncmp(*e, "LC_ALL=", 7) != 0 && std::strncmp(*e, "LANG=", 5) != 0) envStore.push_back(*e);
  envStore.push_back("LC_ALL=C");
  std::vector<char*> cenv;
  for (auto& s : envStore) cenv.push_back(s.data());
  cenv.push_back(nullptr);
  std::vector<std::string> a = args;
  std::vector<char*> cargv;
  for (auto& s : a) cargv.push_back(s.data());
  cargv.push_back(nullptr);

  pid_t pid = 0;
  int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), cenv.data());
  posix_spawn_file_actions_destroy(&fa);
  ::close(pipefd[1]);
  if (rc != 0) {
    ::close(pipefd[0]);
    return r;
  }

  double deadline = nowSeconds() + timeoutSec;
  bool timedOut = false;
  char buf[4096];
  for (;;) {
    double left = deadline - nowSeconds();
    if (left <= 0 || g_cancel) {
      timedOut = true;
      break;
    }
    pollfd p{pipefd[0], POLLIN, 0};
    int n = poll(&p, 1, static_cast<int>(std::min(left, 1.0) * 1000) + 1);
    if (n < 0 && errno == EINTR) continue;
    if (n > 0) {
      ssize_t k = ::read(pipefd[0], buf, sizeof buf);
      if (k > 0) {
        if (r.out.size() < (1u << 20)) r.out.append(buf, static_cast<size_t>(k));
        continue;
      }
      break;  // EOF (or error): the child closed its end
    }
  }
  ::close(pipefd[0]);
  if (timedOut) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  if (timedOut) r.code = -2;
  else if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
  else r.code = -1;

  // bluetoothctl decorates output with ANSI colour and readline markers.
  std::string clean;
  for (size_t i = 0; i < r.out.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(r.out[i]);
    if (c == 0x1b && i + 1 < r.out.size() && r.out[i + 1] == '[') {
      i += 2;
      while (i < r.out.size() && !(r.out[i] >= '@' && r.out[i] <= '~')) ++i;
    } else if (c == 0x01 || c == 0x02 || c == '\r') {
      continue;
    } else {
      clean += static_cast<char>(c);
    }
  }
  r.out = std::move(clean);
  return r;
}

std::string firstLine(const std::string& s) {
  auto l = lines(s);
  return l.empty() ? std::string() : trim(l[0]);
}

// ---- NetworkManager + BlueZ --------------------------------------------------

class SystemNetCtl : public NetCtl {
 public:
  const char* name() const override { return "networkmanager+bluez"; }

  NetStatus status() override {
    NetStatus s;
    CmdResult d = runCmd({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"}, 6);
    if (d.code != 0) return s;
    s.supported = true;
    std::string wifiDev, wiredDev;
    for (auto& l : lines(d.out)) {
      auto f = splitTerse(l);
      if (f.size() < 4) continue;
      bool up = f[2] == "connected";
      if (f[1] == "wifi") {
        s.wifiPresent = true;
        if (wifiDev.empty() || up) wifiDev = f[0];
        if (up) s.ssid = f[3];
      } else if (f[1] == "ethernet" && up) {
        s.wiredConnected = true;
        wiredDev = f[0];
      }
    }
    CmdResult r = runCmd({"nmcli", "-t", "radio", "wifi"}, 4);
    s.wifiEnabled = firstLine(r.out) == "enabled";
    if (!s.ssid.empty()) s.wifiIp = ip4(wifiDev);
    if (s.wiredConnected) s.wiredIp = ip4(wiredDev);
    s.online = !s.ssid.empty() || s.wiredConnected;
    s.knownWifi = static_cast<int>(knownWifi().size());
    return s;
  }

  std::vector<WifiNet> wifiList(bool rescan) override {
    if (rescan) runCmd({"nmcli", "device", "wifi", "rescan"}, 12);  // "too soon since last scan" is fine
    CmdResult r = runCmd({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list"}, 10);
    std::vector<WifiNet> out;
    if (r.code != 0) return out;
    std::set<std::string> known = knownWifi();
    std::map<std::string, size_t> idx;
    for (auto& l : lines(r.out)) {
      auto f = splitTerse(l);
      if (f.size() < 4 || f[1].empty()) continue;  // hidden APs have no SSID
      WifiNet n;
      n.ssid = f[1];
      n.signal = std::atoi(f[2].c_str());
      n.security = (f[3] == "--" || f[3].empty()) ? "" : f[3];
      n.active = f[0] == "*";
      n.known = known.count(n.ssid) > 0;
      auto it = idx.find(n.ssid);
      if (it == idx.end()) {
        idx[n.ssid] = out.size();
        out.push_back(n);
      } else {  // same SSID on several APs: keep the strongest
        WifiNet& e = out[it->second];
        e.signal = std::max(e.signal, n.signal);
        e.active = e.active || n.active;
      }
    }
    std::stable_sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) {
      if (a.active != b.active) return a.active;
      if (a.known != b.known) return a.known;
      return a.signal > b.signal;
    });
    return out;
  }

  NetOp wifiEnable(bool on) override {
    if (on) runCmd({"rfkill", "unblock", "wifi"}, 4);
    CmdResult r = runCmd({"nmcli", "radio", "wifi", on ? "on" : "off"}, 8);
    return r.code == 0 ? good() : fail(wifiError(r));
  }

  NetOp wifiConnect(const std::string& ssid, const std::string& password, bool hidden) override {
    if (ssid.empty() || ssid[0] == '-') return fail("UNSUPPORTED NETWORK NAME");
    if (!password.empty() && (password.size() < 8 || password.size() > 64)) return fail("PASSWORD MUST BE 8 TO 63 CHARACTERS");
    bool had = knownWifi().count(ssid) > 0;
    CmdResult r;
    if (had && password.empty()) {
      r = runCmd({"nmcli", "connection", "up", "id", ssid}, 45);
    } else {
      if (had) runCmd({"nmcli", "connection", "delete", "id", ssid}, 8);  // new password replaces the old profile
      std::vector<std::string> a = {"nmcli", "device", "wifi", "connect", ssid};
      if (!password.empty()) {
        a.push_back("password");
        a.push_back(password);
      }
      if (hidden) {
        a.push_back("hidden");
        a.push_back("yes");
      }
      r = runCmd(a, 60);
    }
    if (r.code == 0) return good();
    // A failed first attempt leaves a half-made profile that would otherwise be
    // retried forever with the wrong secret.
    if (!had || !password.empty()) runCmd({"nmcli", "connection", "delete", "id", ssid}, 8);
    return fail(wifiError(r));
  }

  NetOp wifiDisconnect(const std::string& ssid) override {
    CmdResult r = runCmd({"nmcli", "connection", "down", "id", ssid}, 15);
    return r.code == 0 ? good() : fail(wifiError(r));
  }

  NetOp wifiForget(const std::string& ssid) override {
    CmdResult r = runCmd({"nmcli", "connection", "delete", "id", ssid}, 10);
    return r.code == 0 ? good() : fail(wifiError(r));
  }

  // ---- Bluetooth ----
  BtStatus btStatus() override {
    BtStatus s;
    CmdResult r = runCmd({"bluetoothctl", "show"}, 6);
    if (r.code != 0 || r.out.find("No default controller") != std::string::npos || r.out.find("Controller ") == std::string::npos)
      return s;
    s.supported = true;
    for (auto& l : lines(r.out)) {
      std::string t = trim(l);
      if (t.rfind("Powered:", 0) == 0) s.powered = t.find("yes") != std::string::npos;
      else if (t.rfind("Alias:", 0) == 0) s.name = trim(t.substr(6));
    }
    return s;
  }

  NetOp btPower(bool on) override {
    if (on) runCmd({"rfkill", "unblock", "bluetooth"}, 4);
    CmdResult r = runCmd({"bluetoothctl", "power", on ? "on" : "off"}, 8);
    if (r.code == 0 && r.out.find("Failed") == std::string::npos) return good();
    return fail(btError(r));
  }

  void btScan(int seconds) override {
    runCmd({"bluetoothctl", "--timeout", std::to_string(seconds), "scan", "on"}, seconds + 6);
  }

  std::vector<BtDevice> btDevices() override {
    std::vector<BtDevice> out;
    CmdResult r = runCmd({"bluetoothctl", "devices"}, 6);
    if (r.code != 0) return out;
    for (auto& l : lines(r.out)) {
      if (l.rfind("Device ", 0) != 0 || l.size() < 24) continue;
      BtDevice d;
      d.mac = l.substr(7, 17);
      if (!validMac(d.mac)) continue;
      d.name = trim(l.substr(24));
      CmdResult i = runCmd({"bluetoothctl", "info", d.mac}, 6);
      for (auto& il : lines(i.out)) {
        std::string t = trim(il);
        auto val = [&](const char* key) -> std::string {
          size_t n = std::strlen(key);
          return t.compare(0, n, key) == 0 ? trim(t.substr(n)) : std::string();
        };
        if (auto v = val("Alias:"); !v.empty()) d.name = v;
        else if (auto v2 = val("Icon:"); !v2.empty()) d.icon = v2;
        else if (auto v3 = val("Paired:"); !v3.empty()) d.paired = v3 == "yes";
        else if (auto v4 = val("Trusted:"); !v4.empty()) d.trusted = v4 == "yes";
        else if (auto v5 = val("Connected:"); !v5.empty()) d.connected = v5 == "yes";
        else if (auto v6 = val("Battery Percentage:"); !v6.empty()) {
          size_t p = v6.find('(');
          if (p != std::string::npos) d.battery = std::atoi(v6.c_str() + p + 1);
        }
      }
      // Unnamed devices are beacons and phones passing by: an alias that is just
      // the address with dashes is noise unless we already know the device.
      std::string dashed = d.mac;
      std::replace(dashed.begin(), dashed.end(), ':', '-');
      if (!d.paired && (d.name.empty() || d.name == dashed)) continue;
      out.push_back(std::move(d));
    }
    std::stable_sort(out.begin(), out.end(), [](const BtDevice& a, const BtDevice& b) {
      if (a.connected != b.connected) return a.connected;
      if (a.paired != b.paired) return a.paired;
      if (a.isInput() != b.isInput()) return a.isInput();
      return a.name < b.name;
    });
    return out;
  }

  NetOp btPair(const std::string& mac) override {
    if (!validMac(mac)) return fail("BAD DEVICE ADDRESS");
    // NoInputNoOutput = "just works" pairing, which is what controllers use.
    CmdResult r = runCmd({"bluetoothctl", "--agent", "NoInputNoOutput", "pair", mac}, 35);
    if (r.code != 0 || r.out.find("Failed to pair") != std::string::npos) return fail(btError(r));
    runCmd({"bluetoothctl", "trust", mac}, 8);  // so it reconnects by itself next time
    return btConnect(mac);
  }

  NetOp btConnect(const std::string& mac) override {
    if (!validMac(mac)) return fail("BAD DEVICE ADDRESS");
    CmdResult r = runCmd({"bluetoothctl", "connect", mac}, 25);
    if (r.code == 0 && r.out.find("Failed to connect") == std::string::npos) return good();
    return fail(btError(r));
  }

  NetOp btDisconnect(const std::string& mac) override {
    if (!validMac(mac)) return fail("BAD DEVICE ADDRESS");
    CmdResult r = runCmd({"bluetoothctl", "disconnect", mac}, 12);
    return r.code == 0 ? good() : fail(btError(r));
  }

  NetOp btRemove(const std::string& mac) override {
    if (!validMac(mac)) return fail("BAD DEVICE ADDRESS");
    CmdResult r = runCmd({"bluetoothctl", "remove", mac}, 12);
    return r.code == 0 ? good() : fail(btError(r));
  }

  bool canPower() const override {
    std::error_code ec;
    return fs::exists("/run/systemd/system", ec);
  }

  NetOp power(bool reboot) override {
    CmdResult r = runCmd({"systemctl", reboot ? "reboot" : "poweroff"}, 15);
    return r.code == 0 ? good() : fail("NOT ALLOWED");
  }

 private:
  static std::string ip4(const std::string& dev) {
    if (dev.empty()) return {};
    CmdResult r = runCmd({"nmcli", "-t", "-g", "IP4.ADDRESS", "device", "show", dev}, 5);
    std::string a = firstLine(r.out);
    size_t s = a.find('/');
    return s == std::string::npos ? a : a.substr(0, s);
  }

  static std::set<std::string> knownWifi() {
    std::set<std::string> out;
    CmdResult r = runCmd({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"}, 6);
    if (r.code != 0) return out;
    for (auto& l : lines(r.out)) {
      auto f = splitTerse(l);
      if (f.size() >= 2 && (f[1] == "802-11-wireless" || f[1] == "wifi")) out.insert(f[0]);
    }
    return out;
  }

  static std::string wifiError(const CmdResult& r) {
    if (r.code == -2) return "TIMED OUT";
    if (r.code == -1) return "NETWORK MANAGER UNAVAILABLE";
    const std::string& o = r.out;
    auto has = [&](const char* s) { return o.find(s) != std::string::npos; };
    if (has("Secrets were required") || has("802-11-wireless-security") || has("Invalid password") || has("4-way handshake"))
      return "WRONG PASSWORD";
    if (has("No network with SSID")) return "NETWORK NOT FOUND";
    if (has("rfkill") || has("Wi-Fi is disabled") || has("radio is off")) return "WI-FI IS OFF";
    if (has("No suitable device") || has("device not found") || has("No Wi-Fi device")) return "NO WI-FI ADAPTER";
    if (has("not authorized") || has("Not authorized")) return "NOT ALLOWED";
    if (has("Timeout")) return "TIMED OUT";
    std::string l = upper(firstLine(o));
    if (l.rfind("ERROR: ", 0) == 0) l = l.substr(7);
    return l.empty() ? "FAILED" : l.substr(0, 64);
  }

  static std::string btError(const CmdResult& r) {
    if (r.code == -2) return "TIMED OUT. IS IT IN PAIRING MODE?";
    if (r.code == -1) return "BLUETOOTH UNAVAILABLE";
    const std::string& o = r.out;
    auto has = [&](const char* s) { return o.find(s) != std::string::npos; };
    if (has("AuthenticationFailed") || has("AuthenticationRejected") || has("AuthenticationCanceled")) return "PAIRING WAS REFUSED";
    if (has("AuthenticationTimeout") || has("ConnectionAttemptFailed") || has("page-timeout") || has("Timeout"))
      return "NO RESPONSE. IS IT IN PAIRING MODE?";
    if (has("Blocked")) return "BLUETOOTH IS BLOCKED";
    if (has("AlreadyExists")) return "ALREADY PAIRED";
    if (has("Not available") || has("No default controller")) return "NO BLUETOOTH ADAPTER";
    if (has("not available")) return "DEVICE OUT OF RANGE";
    std::string l = upper(firstLine(o));
    return l.empty() ? "FAILED" : l.substr(0, 64);
  }
};

#endif  // !_WIN32

// ---- no backend (macOS / Windows without --demo) -------------------------------

class UnsupportedNetCtl : public NetCtl {
 public:
  const char* name() const override { return "none"; }
  NetStatus status() override { return {}; }
  std::vector<WifiNet> wifiList(bool) override { return {}; }
  NetOp wifiEnable(bool) override { return no(); }
  NetOp wifiConnect(const std::string&, const std::string&, bool) override { return no(); }
  NetOp wifiDisconnect(const std::string&) override { return no(); }
  NetOp wifiForget(const std::string&) override { return no(); }
  BtStatus btStatus() override { return {}; }
  NetOp btPower(bool) override { return no(); }
  void btScan(int) override {}
  std::vector<BtDevice> btDevices() override { return {}; }
  NetOp btPair(const std::string&) override { return no(); }
  NetOp btConnect(const std::string&) override { return no(); }
  NetOp btDisconnect(const std::string&) override { return no(); }
  NetOp btRemove(const std::string&) override { return no(); }
  bool canPower() const override { return false; }
  NetOp power(bool) override { return no(); }

 private:
  static NetOp no() { return fail("NOT AVAILABLE ON THIS SYSTEM"); }
};

// ---- fixtures for --demo --------------------------------------------------------

class DemoNetCtl : public NetCtl {
 public:
  DemoNetCtl() {
    nets_ = {{"VolcanoLounge", "WPA2", 88, true, true}, {"BT-Hub-7QK2", "WPA2", 71, false, false},
             {"Neighbour_5G", "WPA3", 54, false, false},  {"CoffeeShop Guest", "", 47, false, false},
             {"VM1234567", "WPA2", 33, false, false}};
    bt_ = {{"E4:17:D8:00:11:22", "Xbox Wireless Controller", "input-gaming", true, true, true, 82},
           {"98:B6:E9:44:55:66", "Pro Controller", "input-gaming", true, false, true, -1},
           {"CC:C0:79:AA:BB:CC", "Wireless Controller", "input-gaming", false, false, false, -1},
           {"70:BB:E9:12:34:56", "JBL Flip 5", "audio-card", false, false, false, -1},
           {"DC:2C:26:77:88:99", "K380 Keyboard", "input-keyboard", false, false, false, -1}};
  }
  const char* name() const override { return "demo"; }

  NetStatus status() override {
    nap(250);
    std::lock_guard<std::mutex> lk(m_);
    NetStatus s;
    s.supported = s.wifiPresent = true;
    s.wifiEnabled = wifiOn_;
    for (auto& n : nets_) {
      if (n.known) ++s.knownWifi;
      if (n.active && wifiOn_) {
        s.ssid = n.ssid;
        s.wifiIp = "192.168.1.42";
        s.online = true;
      }
    }
    return s;
  }
  std::vector<WifiNet> wifiList(bool rescan) override {
    nap(rescan ? 1400 : 150);
    std::lock_guard<std::mutex> lk(m_);
    return wifiOn_ ? nets_ : std::vector<WifiNet>{};
  }
  NetOp wifiEnable(bool on) override {
    nap(500);
    std::lock_guard<std::mutex> lk(m_);
    wifiOn_ = on;
    return good();
  }
  NetOp wifiConnect(const std::string& ssid, const std::string& pw, bool) override {
    nap(1600);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& n : nets_) {
      if (n.ssid != ssid) continue;
      if (n.secured() && !n.known && pw != "volcano123") return fail("WRONG PASSWORD");
      for (auto& o : nets_) o.active = false;
      n.active = n.known = true;
      return good();
    }
    return fail("NETWORK NOT FOUND");
  }
  NetOp wifiDisconnect(const std::string& ssid) override {
    nap(400);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& n : nets_)
      if (n.ssid == ssid) n.active = false;
    return good();
  }
  NetOp wifiForget(const std::string& ssid) override {
    nap(300);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& n : nets_)
      if (n.ssid == ssid) n.active = n.known = false;
    return good();
  }

  BtStatus btStatus() override {
    nap(150);
    std::lock_guard<std::mutex> lk(m_);
    return {true, btOn_, "volcano"};
  }
  NetOp btPower(bool on) override {
    nap(400);
    std::lock_guard<std::mutex> lk(m_);
    btOn_ = on;
    if (!on)
      for (auto& d : bt_) d.connected = false;
    return good();
  }
  void btScan(int seconds) override { nap(std::min(seconds, 3) * 1000); }
  std::vector<BtDevice> btDevices() override {
    nap(150);
    std::lock_guard<std::mutex> lk(m_);
    std::vector<BtDevice> out;
    if (!btOn_) return out;
    for (auto& d : bt_)
      if (d.paired || scanned_) out.push_back(d);
    std::stable_sort(out.begin(), out.end(), [](const BtDevice& a, const BtDevice& b) {
      if (a.connected != b.connected) return a.connected;
      return a.paired && !b.paired;
    });
    return out;
  }
  NetOp btPair(const std::string& mac) override {
    nap(2200);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& d : bt_)
      if (d.mac == mac) {
        if (d.icon == "audio-card") return fail("NO RESPONSE. IS IT IN PAIRING MODE?");
        d.paired = d.trusted = d.connected = true;
      }
    return good();
  }
  NetOp btConnect(const std::string& mac) override {
    nap(1200);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& d : bt_)
      if (d.mac == mac) d.connected = true;
    return good();
  }
  NetOp btDisconnect(const std::string& mac) override {
    nap(500);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& d : bt_)
      if (d.mac == mac) d.connected = false;
    return good();
  }
  NetOp btRemove(const std::string& mac) override {
    nap(400);
    std::lock_guard<std::mutex> lk(m_);
    for (auto& d : bt_)
      if (d.mac == mac) d.paired = d.trusted = d.connected = false;
    return good();
  }
  bool canPower() const override { return true; }
  NetOp power(bool) override { return fail("DEMO MODE"); }

 private:
  static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
  std::mutex m_;
  bool wifiOn_ = true, btOn_ = true, scanned_ = true;
  std::vector<WifiNet> nets_;
  std::vector<BtDevice> bt_;
};

}  // namespace

void netctlShutdown() {
#ifndef _WIN32
  g_cancel = true;
#endif
}

std::unique_ptr<NetCtl> createNetCtl(bool demo) {
  if (demo) return std::make_unique<DemoNetCtl>();
#ifndef _WIN32
  // VOS_NETCTL=system forces the real backend off-Linux (used to test against fake nmcli/bluetoothctl scripts).
#ifdef __linux__
  return std::make_unique<SystemNetCtl>();
#else
  const char* force = std::getenv("VOS_NETCTL");
  if (force && std::string(force) == "system") return std::make_unique<SystemNetCtl>();
#endif
#endif
  return std::make_unique<UnsupportedNetCtl>();
}

}  // namespace vos
