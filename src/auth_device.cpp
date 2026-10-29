#include "auth_device.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

#include "http.hpp"
#include "json.hpp"
#include "util.hpp"

namespace vos {

namespace {

const char* kClientId = "zonto-launcher";
const char* kGrantType = "urn:ietf:params:oauth:grant-type:device_code";
constexpr int kMaxCodes = 3;  // codes last 15 min; stop asking after ~45 min of nobody approving

using nlohmann::json;

std::string str(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
double num(const json& j, const char* k, double d) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number()) ? it->get<double>() : d;
}

}  // namespace

DeviceSignIn::~DeviceSignIn() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
}

DeviceSignIn::Code DeviceSignIn::code() const {
  std::lock_guard<std::mutex> l(m_);
  return code_;
}
std::string DeviceSignIn::token() const {
  std::lock_guard<std::mutex> l(m_);
  return token_;
}
Credentials DeviceSignIn::credentials() const {
  std::lock_guard<std::mutex> l(m_);
  return creds_;
}
std::string DeviceSignIn::error() const {
  std::lock_guard<std::mutex> l(m_);
  return error_;
}

void DeviceSignIn::cancel() {
  cancel_.store(true);
  if (state() == State::Starting || state() == State::Waiting) setState(State::Cancelled);
}

bool DeviceSignIn::sleepFor(double seconds) {
  double end = nowSeconds() + seconds;
  while (nowSeconds() < end) {
    if (cancel_.load()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !cancel_.load();
}

void DeviceSignIn::reset() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
  setState(State::Idle);
}

void DeviceSignIn::startDemo() {
  if (thread_.joinable()) {
    cancel_.store(true);
    thread_.join();
  }
  cancel_.store(false);
  std::lock_guard<std::mutex> l(m_);
  code_ = {"WXYZ-2345", "https://www.nikorag.co.uk/device", "https://www.nikorag.co.uk/device?user_code=WXYZ-2345", nowSeconds() + 872};
  token_.clear();
  error_.clear();
  setState(State::Waiting);
}

void DeviceSignIn::start(const std::string& apiBase, const std::string& deviceName) {
  if (thread_.joinable()) {
    cancel_.store(true);
    thread_.join();
  }
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> l(m_);
    code_ = {};
    token_.clear();
    error_.clear();
  }
  setState(State::Starting);
  thread_ = std::thread([this, apiBase, deviceName] { run(apiBase, deviceName); });
}

void DeviceSignIn::run(std::string base, std::string deviceName) {
  // A late update from this thread must not undo a cancel() made on the UI thread.
  auto put = [this](State s) {
    if (!cancel_.load()) setState(s);
  };
  auto fail = [this, &put](const std::string& why) {
    {
      std::lock_guard<std::mutex> l(m_);
      error_ = why;
    }
    put(State::Failed);
  };
  auto cancelled = [this] { return cancel_.load(); };

  for (int attempt = 0; attempt < kMaxCodes; ++attempt) {
    if (cancelled()) return;
    put(State::Starting);

    // ---- ask for a code ----
    HttpRequest req;
    req.method = "POST";
    req.url = base + "/api/oauth/device";
    req.contentType = "application/json";
    req.timeoutSec = 15;
    req.body = json{{"client_id", kClientId}, {"device_name", deviceName}}.dump();
    HttpResponse res = httpRequest(req);
    if (cancelled()) return;
    if (res.status == 0) return fail("Can't reach Zonto (" + res.error + ")");
    if (res.status == 404 || res.status == 405) {  // server predates the device flow
      put(State::Unsupported);
      return;
    }
    if (res.status == 429) return fail("Too many sign-in attempts. Wait a few minutes and try again.");
    json j = json::parse(res.body, nullptr, false);
    if (!res.ok() || !j.is_object()) return fail("Couldn't get a sign-in code (HTTP " + std::to_string(res.status) + ")");
    std::string deviceCode = str(j, "deviceCode");
    Code c;
    c.userCode = str(j, "userCode");
    c.verificationUrl = str(j, "verificationUrl");
    c.verificationUrlComplete = str(j, "verificationUrlComplete");
    if (c.verificationUrlComplete.empty()) c.verificationUrlComplete = c.verificationUrl;
    if (deviceCode.empty() || c.userCode.empty() || c.verificationUrlComplete.empty()) return fail("Unreadable sign-in response.");
    if (c.userCode.size() == 8 && c.userCode.find('-') == std::string::npos) c.userCode.insert(4, "-");
    double expiresIn = std::max(30.0, num(j, "expiresIn", 900));
    // The server never reports a raised interval, so we keep our own (see slow_down below).
    double interval = std::min(30.0, std::max(1.0, num(j, "interval", 5)));
    c.expiresAt = nowSeconds() + expiresIn;
    {
      std::lock_guard<std::mutex> l(m_);
      code_ = c;
    }
    put(State::Waiting);

    // ---- poll until approved, refused or expired ----
    bool expired = false;
    while (!expired) {
      // First poll after one interval: an immediate one can only say "pending".
      if (!sleepFor(interval)) return;
      if (nowSeconds() >= c.expiresAt) break;

      HttpRequest poll;
      poll.method = "POST";
      poll.url = base + "/api/oauth/token";
      poll.contentType = "application/json";
      poll.timeoutSec = 15;
      poll.body = json{{"grant_type", kGrantType}, {"device_code", deviceCode}, {"client_id", kClientId}}.dump();
      HttpResponse r = httpRequest(poll);
      if (cancelled()) return;

      if (r.status == 0 || r.status >= 500) continue;  // network blip or server hiccup: keep going until the code expires
      json pj = json::parse(r.body, nullptr, false);
      if (r.ok()) {
        std::string tok = pj.is_object() ? str(pj, "access_token") : std::string();
        if (tok.empty()) return fail("Unreadable token response.");
        {
          std::lock_guard<std::mutex> l(m_);
          token_ = tok;
          creds_ = {};
          creds_.access = tok;
          creds_.refresh = str(pj, "refresh_token");
          // No expires_in means "unknown": the 401 path will still refresh when it matters.
          if (num(pj, "expires_in", 0) > 0) creds_.expiresAt = static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(num(pj, "expires_in", 0));
        }
        put(State::Done);
        return;
      }
      std::string err = pj.is_object() ? str(pj, "error") : std::string();
      if (err == "authorization_pending") continue;
      if (err == "slow_down") {
        interval = std::min(60.0, interval + 5);  // RFC 8628 s3.5; the server restarts its clock on this poll too
        continue;
      }
      if (err == "access_denied") return fail("Sign-in was declined on your phone.");
      if (err == "expired_token") {
        expired = true;
        break;
      }
      if (err == "invalid_client") return fail("This launcher isn't allowed to use phone sign-in.");
      return fail("Sign-in failed (" + (err.empty() ? "HTTP " + std::to_string(r.status) : err) + ")");
    }
    // Expired: loop round and fetch a fresh code without the player doing anything.
  }
  fail("Nobody approved the code. Press A to get a new one.");
}

}  // namespace vos
