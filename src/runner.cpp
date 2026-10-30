#include "runner.hpp"

#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" char** environ;

namespace vos {

static std::vector<std::string> splitArgs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  std::string t;
  while (is >> t) out.push_back(t);
  return out;
}

GameProcess::~GameProcess() {
  // Deliberately not terminating: closing the launcher must not kill a game.
}

#ifdef _WIN32

bool GameProcess::spawn(const fs::path& exe, const fs::path& workdir, const std::string& args, const fs::path&,
                        std::string& err, const Env& env) {
  for (auto& [k, v] : env) _putenv_s(k.c_str(), v.c_str());  // inherited by the child
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  for (auto& a : splitArgs(args)) cmd += L" " + std::wstring(a.begin(), a.end());
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workdir.wstring().c_str(), &si, &pi)) {
    err = "could not start the game (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  handle_ = pi.hProcess;
  valid_ = true;
  return true;
}

bool GameProcess::poll(int& exitCode) {
  if (!valid_) return false;
  if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    valid_ = false;
    exitCode = static_cast<int>(code);
    return true;
  }
  return false;
}

void GameProcess::terminate() {
  if (valid_) TerminateProcess(static_cast<HANDLE>(handle_), 1);
}

void GameProcess::forceKill() { terminate(); }  // already unconditional on Windows

#else

bool GameProcess::spawn(const fs::path& exe, const fs::path& workdir, const std::string& args, const fs::path& logFile,
                        std::string& err, const Env& env) {
  std::vector<std::string> argv;
  auto extra = splitArgs(args);
#ifdef __APPLE__
  if (exe.extension() == ".app") {
    argv = {"/usr/bin/open", "-W", "-a", exe.string()};
    for (auto& [k, v] : env) {  // LaunchServices doesn't inherit our environment
      argv.push_back("--env");
      argv.push_back(k + "=" + v);
    }
    if (!extra.empty()) {
      argv.push_back("--args");
      argv.insert(argv.end(), extra.begin(), extra.end());
    }
  } else
#endif
  {
    argv.push_back(exe.string());
    argv.insert(argv.end(), extra.begin(), extra.end());
  }
  std::vector<char*> cargv;
  for (auto& a : argv) cargv.push_back(a.data());
  cargv.push_back(nullptr);
  // Build the child's environment before fork(): allocating after fork in a
  // multithreaded process is unsafe.
  std::vector<std::string> envStore;
  for (char** e = environ; e && *e; ++e) envStore.push_back(*e);
  for (auto& [k, v] : env) envStore.push_back(k + "=" + v);
  std::vector<char*> cenv;
  for (auto& s : envStore) cenv.push_back(s.data());
  cenv.push_back(nullptr);

  std::error_code ec;
  fs::create_directories(logFile.parent_path(), ec);
  int logfd = ::open(logFile.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::string wd = workdir.string();

  pid_t pid = fork();
  if (pid < 0) {
    if (logfd >= 0) ::close(logfd);
    err = "could not start the game (fork failed)";
    return false;
  }
  if (pid == 0) {
    // Child: only async-signal-safe calls from here to exec.
    if (logfd >= 0) {
      dup2(logfd, STDOUT_FILENO);
      dup2(logfd, STDERR_FILENO);
      ::close(logfd);
    }
    if (chdir(wd.c_str()) != 0) _exit(126);
    execve(cargv[0], cargv.data(), cenv.data());
    _exit(127);
  }
  if (logfd >= 0) ::close(logfd);
  pid_ = pid;
  valid_ = true;
  return true;
}

bool GameProcess::poll(int& exitCode) {
  if (!valid_) return false;
  int status = 0;
  pid_t r = waitpid(pid_, &status, WNOHANG);
  if (r == 0) return false;
  valid_ = false;
  if (r < 0) exitCode = -1;
  else if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) exitCode = 128 + WTERMSIG(status);
  else exitCode = -1;
  return true;
}

void GameProcess::terminate() {
  if (valid_) kill(pid_, SIGTERM);
}

void GameProcess::forceKill() {
  if (valid_) kill(pid_, SIGKILL);
}

#endif

}  // namespace vos
