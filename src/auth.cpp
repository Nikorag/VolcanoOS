#include "auth.hpp"

#include <SDL.h>

#include <chrono>
#include <cstring>
#include <map>

#include "http.hpp"
#include "json.hpp"
#include "sha256.hpp"
#include "util.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
static const sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { close(s); }
static const sock_t kBadSock = -1;
#endif

namespace vos {

namespace {

const char* kClientId = "zonto-launcher";
constexpr double kLoginTimeoutSec = 300;

std::string pctDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
      o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < q.size()) {
    size_t amp = q.find('&', i);
    std::string pair = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) out[pctDecode(pair.substr(0, eq))] = pctDecode(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

void respondHtml(sock_t c, const std::string& title, const std::string& message) {
  std::string body =
      "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + title +
      "</title><style>body{background:#0b0908;color:#f4ece4;font-family:ui-monospace,Menlo,monospace;"
      "display:grid;place-items:center;height:100vh;margin:0}div{text-align:center;line-height:1.9}"
      "b{color:#ff6a1f;letter-spacing:.24em;font-size:13px}p{color:#96867a;font-size:13px}</style></head>"
      "<body><div><b>" + title + "</b><p>" + message + "</p></div></body></html>";
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  send(c, resp.data(), static_cast<int>(resp.size()), 0);
}

}  // namespace

SignInFlow::~SignInFlow() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
}

std::string SignInFlow::authorizeUrl() const {
  std::lock_guard<std::mutex> l(m_);
  return url_;
}
std::string SignInFlow::token() const {
  std::lock_guard<std::mutex> l(m_);
  return token_;
}
std::string SignInFlow::error() const {
  std::lock_guard<std::mutex> l(m_);
  return error_;
}

void SignInFlow::cancel() {
  cancel_.store(true);
}

void SignInFlow::start(const std::string& apiBase) {
  if (thread_.joinable()) {
    cancel_.store(true);
    thread_.join();
  }
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> l(m_);
    url_.clear();
    token_.clear();
    error_.clear();
  }
  setState(State::Waiting);
  thread_ = std::thread([this, apiBase] { run(apiBase); });
}

void SignInFlow::run(std::string base) {
  auto fail = [this](const std::string& why) {
    {
      std::lock_guard<std::mutex> l(m_);
      error_ = why;
    }
    setState(State::Failed);
  };

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  // PKCE: 32 random bytes -> 43 base64url chars, over the server's 40-char floor.
  std::string verifier = randomToken(32);
  std::string state = randomToken(16);
  if (verifier.empty() || state.empty()) return fail("No secure randomness available.");
  std::string challenge;
  {
    auto d = Sha256::of(verifier);
    challenge = base64Url(d.data(), d.size());
  }

  // Port 0: the OS picks a free port; the server accepts any loopback port.
  sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls == kBadSock) return fail("Could not open a loopback port for sign-in.");
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, 4) != 0) {
    closeSock(ls);
    return fail("Could not listen on a loopback port.");
  }
  socklen_t alen = sizeof addr;
  getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen);
  int port = ntohs(addr.sin_port);
  std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/cb";

  std::string url = base + "/oauth/authorize?client_id=" + kClientId + "&redirect_uri=" + urlEncode(redirect) +
                    "&response_type=code&state=" + urlEncode(state) + "&code_challenge=" + urlEncode(challenge) +
                    "&code_challenge_method=S256";
  {
    std::lock_guard<std::mutex> l(m_);
    url_ = url;
  }
  if (SDL_OpenURL(url.c_str()) != 0) logf("could not open a browser automatically: %s", SDL_GetError());

  // Serve requests until /cb arrives (favicon etc. are answered and ignored).
  std::map<std::string, std::string> cb;
  bool got = false;
  double deadline = nowSeconds() + kLoginTimeoutSec;
  while (!got) {
    if (cancel_.load()) {
      closeSock(ls);
      setState(State::Cancelled);
      return;
    }
    if (nowSeconds() > deadline) {
      closeSock(ls);
      return fail("Timed out waiting for the browser sign-in.");
    }
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    timeval tv{0, 200000};
    if (select(static_cast<int>(ls) + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
    sock_t c = accept(ls, nullptr, nullptr);
    if (c == kBadSock) continue;
    timeval rt{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rt), sizeof rt);
    char buf[4096];
    int n = static_cast<int>(recv(c, buf, sizeof buf - 1, 0));
    if (n > 0) {
      buf[n] = 0;
      std::string req(buf);
      // "GET /cb?code=...&state=... HTTP/1.1"
      size_t sp1 = req.find(' ');
      size_t sp2 = req.find(' ', sp1 + 1);
      std::string target = (sp1 != std::string::npos && sp2 != std::string::npos) ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
      if (target.rfind("/cb", 0) == 0) {
        size_t q = target.find('?');
        cb = parseQuery(q == std::string::npos ? "" : target.substr(q + 1));
        if (cb.count("code")) respondHtml(c, "SIGNED IN", "You can close this tab and return to Volcano OS.");
        else respondHtml(c, "SIGN-IN CANCELLED", "Nothing was changed. You can close this tab.");
        got = true;
      } else {
        respondHtml(c, "NOT FOUND", "This window can be closed.");
      }
    }
    closeSock(c);
  }
  closeSock(ls);

  if (cb.count("error")) return fail(cb["error"] == "access_denied" ? "Sign-in was declined." : "Sign-in failed: " + cb["error"]);
  if (!cb.count("state") || cb["state"] != state) return fail("Sign-in response did not match this request. Try again.");
  if (!cb.count("code")) return fail("Sign-in returned no authorization code.");

  setState(State::Exchanging);
  HttpRequest req;
  req.method = "POST";
  req.url = base + "/api/oauth/token";
  req.contentType = "application/x-www-form-urlencoded";
  req.body = "grant_type=authorization_code&code=" + urlEncode(cb["code"]) + "&client_id=" + kClientId +
             "&redirect_uri=" + urlEncode(redirect) + "&code_verifier=" + urlEncode(verifier);
  HttpResponse res = httpRequest(req);
  if (res.status == 0) return fail("Can't reach Zonto (" + res.error + ")");
  if (!res.ok()) return fail("Token exchange failed (HTTP " + std::to_string(res.status) + ")");
  nlohmann::json j = nlohmann::json::parse(res.body, nullptr, false);
  if (!j.is_object() || !j.contains("access_token") || !j["access_token"].is_string())
    return fail("Unreadable token response.");
  {
    std::lock_guard<std::mutex> l(m_);
    token_ = j["access_token"].get<std::string>();
  }
  setState(State::Done);
}

}  // namespace vos
