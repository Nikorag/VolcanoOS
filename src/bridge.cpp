#include "bridge.hpp"

#include <cstring>

#include "json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
static const sock_t kBad = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { close(s); }
static const sock_t kBad = -1;
#endif

namespace vos {

namespace {

std::string clean(const std::string& s, size_t max) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 32 && c != '\n') continue;  // no control characters
    if (c == '\n') c = ' ';
    o += static_cast<char>(c);
    if (o.size() >= max) break;
  }
  // Don't leave a split UTF-8 sequence at the end.
  while (!o.empty() && (static_cast<unsigned char>(o.back()) & 0xC0) == 0x80) o.pop_back();
  if (!o.empty() && (static_cast<unsigned char>(o.back()) & 0x80)) {
    // lead byte with missing continuation bytes
    size_t i = o.size() - 1;
    int need = (o[i] & 0xE0) == 0xC0 ? 1 : (o[i] & 0xF0) == 0xE0 ? 2 : (o[i] & 0xF8) == 0xF0 ? 3 : 0;
    if (need > 0) o.pop_back();
  }
  return o;
}

bool constantTimeEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

void reply(sock_t c, int code, const char* text, const std::string& body) {
  std::string r = "HTTP/1.1 " + std::to_string(code) + " " + text +
                  "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nConnection: close\r\n\r\n" + body;
  send(c, r.data(), static_cast<int>(r.size()), 0);
}

}  // namespace

bool GameBridge::parse(const std::string& text, GameEvent& out) {
  nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return false;
  auto str = [&](const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
  };
  std::string kind = str("kind");
  if (kind.empty()) kind = "toast";
  if (kind != "toast" && kind != "achievement" && kind != "status") return false;
  out.kind = kind;
  out.title = clean(str("title"), 80);
  out.body = clean(str("body"), 200);
  if (kind == "status") return true;
  return !out.title.empty() || !out.body.empty();
}

bool GameBridge::start(std::function<void(GameEvent)> onEvent) {
  stop();
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  token_ = randomToken(24);
  if (token_.empty()) return false;
  onEvent_ = std::move(onEvent);

  sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls == kBad) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable off-machine
  a.sin_port = 0;
  if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(ls, 8) != 0) {
    closeSock(ls);
    return false;
  }
  socklen_t len = sizeof a;
  getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  listenFd_ = static_cast<int>(ls);
  stop_ = false;
  thread_ = std::thread([this] { serve(); });
  return true;
}

void GameBridge::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (listenFd_ >= 0) closeSock(static_cast<sock_t>(listenFd_));
  listenFd_ = -1;
  port_ = 0;
}

void GameBridge::serve() {
  sock_t ls = static_cast<sock_t>(listenFd_);
  while (!stop_) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    timeval tv{0, 200000};
    if (select(static_cast<int>(ls) + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
    sock_t c = accept(ls, nullptr, nullptr);
    if (c == kBad) continue;
    timeval rt{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rt), sizeof rt);

    // Read headers, then the body per Content-Length (capped: this is a toast channel).
    std::string req;
    char buf[2048];
    size_t headerEnd = std::string::npos;
    while (req.size() < 16384) {
      int n = static_cast<int>(recv(c, buf, sizeof buf, 0));
      if (n <= 0) break;
      req.append(buf, n);
      headerEnd = req.find("\r\n\r\n");
      if (headerEnd != std::string::npos) {
        size_t cl = 0;
        size_t p = req.find("Content-Length:");
        if (p == std::string::npos) p = req.find("content-length:");
        if (p != std::string::npos && p < headerEnd) cl = static_cast<size_t>(std::atoi(req.c_str() + p + 15));
        if (req.size() >= headerEnd + 4 + std::min<size_t>(cl, 8192)) break;
      }
    }
    if (headerEnd == std::string::npos) {
      reply(c, 400, "Bad Request", "{\"error\":\"bad request\"}");
      closeSock(c);
      continue;
    }
    std::string head = req.substr(0, headerEnd), body = req.substr(headerEnd + 4);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    std::string method = head.substr(0, sp1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos) ? head.substr(sp1 + 1, sp2 - sp1 - 1) : "";

    std::string auth;
    for (const char* key : {"Authorization:", "authorization:"}) {
      size_t p = head.find(key);
      if (p != std::string::npos) {
        size_t e = head.find("\r\n", p);
        auth = trim(head.substr(p + std::strlen(key), e == std::string::npos ? std::string::npos : e - p - std::strlen(key)));
        break;
      }
    }
    if (!constantTimeEq(auth, "Bearer " + token_)) {
      reply(c, 401, "Unauthorized", "{\"error\":\"missing or wrong token\"}");
    } else if (method == "GET" && path == "/v1/ping") {
      reply(c, 200, "OK", std::string("{\"ok\":true,\"launcher\":\"volcano-os\",\"version\":\"") + VOS_VERSION + "\"}");
    } else if (method == "POST" && (path == "/v1/notify" || path == "/v1/status")) {
      GameEvent ev;
      // /v1/status is shorthand: {"text": "..."}
      std::string payload = body;
      if (path == "/v1/status") {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::string t = (j.is_object() && j.contains("text") && j["text"].is_string()) ? j["text"].get<std::string>() : "";
        payload = nlohmann::json{{"kind", "status"}, {"body", t}}.dump();
      }
      if (GameBridge::parse(payload, ev)) {
        main_.post([this, ev] {
          if (onEvent_) onEvent_(ev);
        });
        reply(c, 202, "Accepted", "{\"ok\":true}");
      } else {
        reply(c, 400, "Bad Request", "{\"error\":\"invalid event\"}");
      }
    } else {
      reply(c, 404, "Not Found", "{\"error\":\"unknown endpoint\"}");
    }
    closeSock(c);
  }
}

}  // namespace vos
