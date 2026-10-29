// "Sign in on your phone": OAuth 2.0 Device Authorization Grant (RFC 8628).
// The console shows a short code and a QR code; the player approves on their
// phone (where Google sign-in and a password manager work) and the launcher
// picks up the same token the browser flow would have produced. No keyboard
// needed on the device.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "api.hpp"

namespace vos {

class DeviceSignIn {
 public:
  enum class State {
    Idle,
    Starting,     // asking the server for a code
    Waiting,      // code on screen, polling for approval
    Done,         // token() is valid
    Failed,       // error() says why
    Cancelled,
    Unsupported,  // the server has no device-code endpoint: fall back to the browser flow
  };

  struct Code {
    std::string userCode;                 // "WXYZ-2345"
    std::string verificationUrl;          // short page to type the code into
    std::string verificationUrlComplete;  // same with the code prefilled: what the QR encodes
    double expiresAt = 0;                 // nowSeconds() timescale
  };

  ~DeviceSignIn();
  void start(const std::string& apiBase, const std::string& deviceName);
  void startDemo();  // fixture state for --demo and screenshots; no network
  void cancel();
  void reset();      // back to Idle once the UI has consumed Done/Failed/Unsupported

  State state() const { return state_.load(); }
  Code code() const;
  std::string token() const;        // valid once Done
  Credentials credentials() const;  // valid once Done: access + refresh token + expiry
  std::string error() const;        // valid once Failed

 private:
  void run(std::string base, std::string deviceName);
  void setState(State s) { state_.store(s); }
  bool sleepFor(double seconds);  // false if cancelled meanwhile

  std::atomic<State> state_{State::Idle};
  std::atomic<bool> cancel_{false};
  std::thread thread_;
  mutable std::mutex m_;
  Code code_;
  std::string token_, error_;
  Credentials creds_;
};

}  // namespace vos
