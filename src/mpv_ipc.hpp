// Minimal client for mpv's JSON IPC (--input-ipc-server): send commands, read the properties we
// observe. Non-blocking: poll() once per frame. Unix domain sockets only; on Windows it never
// connects and the app plays without remote control.
#pragma once

#include <string>

#include "util.hpp"

namespace vos {

class MpvIpc {
 public:
  MpvIpc() = default;
  MpvIpc(const MpvIpc&) = delete;
  MpvIpc& operator=(const MpvIpc&) = delete;
  ~MpvIpc() { close(); }

  // Try to connect (the socket appears a moment after mpv starts). True once connected.
  bool connect(const std::string& path);
  bool connected() const { return fd_ >= 0; }
  void close();
  void reset() { close(); position = duration = 0; paused = seen = ended = false; buf_.clear(); }

  // Command as a JSON array, e.g. {"seek", 10, "relative"} built by the caller:
  //   cmd(R"(["seek", 10, "relative"])")
  void cmd(const std::string& jsonArray);
  void showText(const std::string& text, int ms = 1500);

  // Drain whatever mpv sent. Updates the observed properties below.
  void poll();

  double position = 0;   // seconds (time-pos)
  double duration = 0;   // seconds
  bool paused = false;
  bool seen = false;     // true once a time-pos has arrived
  bool ended = false;    // end-file received

 private:
  void line(const std::string& l);
  int fd_ = -1;
  std::string buf_;
};

}  // namespace vos
