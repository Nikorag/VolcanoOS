// Supervised child process for native titles.
//
// A macOS .app is a directory, so it can't be exec'd: it goes through `open`,
// whose -W makes it wait for the app to quit — which is what lets the launcher
// keep presence and playtime accurate.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace vos {

class GameProcess {
 public:
  GameProcess() = default;
  GameProcess(const GameProcess&) = delete;
  GameProcess& operator=(const GameProcess&) = delete;
  ~GameProcess();

  using Env = std::vector<std::pair<std::string, std::string>>;
  // `env` is added to the launcher's environment (used to hand the game its overlay-bridge port + token).
  bool spawn(const fs::path& exe, const fs::path& workdir, const std::string& args, const fs::path& logFile,
             std::string& err, const Env& env = {});
  bool running() const { return valid_; }
  // Non-blocking. Returns true once the process has ended and fills exitCode.
  bool poll(int& exitCode);
  void terminate();
  void forceKill();  // SIGKILL: for a process that ignored terminate()

 private:
  bool valid_ = false;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int pid_ = 0;
#endif
};

}  // namespace vos
