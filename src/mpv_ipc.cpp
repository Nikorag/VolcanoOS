#include "mpv_ipc.hpp"

#include <cerrno>
#include <cstdio>

#include "json.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace vos {

using nlohmann::json;

#ifdef _WIN32

bool MpvIpc::connect(const std::string&) { return false; }
void MpvIpc::close() {}
void MpvIpc::cmd(const std::string&) {}
void MpvIpc::showText(const std::string&, int) {}
void MpvIpc::poll() {}
void MpvIpc::line(const std::string&) {}

#else

bool MpvIpc::connect(const std::string& path) {
  if (fd_ >= 0) return true;
  sockaddr_un sa{};
  sa.sun_family = AF_UNIX;
  if (path.size() >= sizeof sa.sun_path) return false;
  std::snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path.c_str());
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    ::close(fd);
    return false;
  }
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  fd_ = fd;
  buf_.clear();
  seen = ended = false;
  // ids are arbitrary; the names are what we match on.
  cmd(R"(["observe_property", 1, "time-pos"])");
  cmd(R"(["observe_property", 2, "duration"])");
  cmd(R"(["observe_property", 3, "pause"])");
  logf("mpv ipc connected");
  return true;
}

void MpvIpc::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

void MpvIpc::cmd(const std::string& jsonArray) {
  if (fd_ < 0) return;
  std::string m = "{\"command\":" + jsonArray + "}\n";
  size_t off = 0;
  while (off < m.size()) {
    ssize_t n = ::send(fd_, m.data() + off, m.size() - off,
#ifdef MSG_NOSIGNAL
                       MSG_NOSIGNAL
#else
                       0
#endif
    );
    if (n <= 0) {  // a full buffer on a local socket means mpv is wedged; drop the command
      if (n < 0 && errno != EAGAIN && errno != EINTR) close();
      return;
    }
    off += static_cast<size_t>(n);
  }
}

static std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

void MpvIpc::showText(const std::string& text, int ms) {
  cmd("[\"show-text\", \"" + jsonEscape(text) + "\", " + std::to_string(ms) + "]");
}

void MpvIpc::poll() {
  if (fd_ < 0) return;
  char tmp[4096];
  for (;;) {
    ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
    if (n > 0) {
      buf_.append(tmp, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) { close(); break; }  // mpv closed the socket
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close();
    break;
  }
  size_t nl;
  while ((nl = buf_.find('\n')) != std::string::npos) {
    line(buf_.substr(0, nl));
    buf_.erase(0, nl + 1);
  }
  if (buf_.size() > (1 << 20)) buf_.clear();
}

void MpvIpc::line(const std::string& l) {
  json j = json::parse(l, nullptr, false);
  if (!j.is_object()) return;
  const std::string ev = j.value("event", "");
  if (ev == "property-change") {
    const std::string name = j.value("name", "");
    auto it = j.find("data");
    if (it == j.end() || it->is_null()) return;
    if (name == "time-pos" && it->is_number()) { position = it->get<double>(); seen = true; }
    else if (name == "duration" && it->is_number()) duration = it->get<double>();
    else if (name == "pause" && it->is_boolean()) paused = it->get<bool>();
  } else if (ev == "end-file") {
    ended = true;
  }
}

#endif

}  // namespace vos
