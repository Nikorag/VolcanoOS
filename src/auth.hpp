// "Sign in with Zonto": OAuth 2.0 authorization code + PKCE over a loopback
// redirect, with the player's real browser doing the login (so Google SSO
// works and no password is ever typed into this app). `zonto-launcher` is a
// first-party client, so there's no consent screen.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "api.hpp"

namespace vos {

class SignInFlow {
 public:
  enum class State { Idle, Waiting, Exchanging, Done, Failed, Cancelled };

  ~SignInFlow();
  void start(const std::string& apiBase);
  void cancel();

  State state() const { return state_.load(); }
  std::string authorizeUrl() const;
  std::string token() const;         // valid once Done
  Credentials credentials() const;   // valid once Done: access + refresh token + expiry
  std::string error() const;         // valid once Failed

 private:
  void run(std::string base);
  void setState(State s) { state_.store(s); }

  std::atomic<State> state_{State::Idle};
  std::atomic<bool> cancel_{false};
  std::thread thread_;
  mutable std::mutex m_;
  std::string url_, token_, error_;
  Credentials creds_;
};

}  // namespace vos
