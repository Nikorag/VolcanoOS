// Native build install/update/uninstall.
//
// Layout (machine-local, never roaming):
//   <data>/games/<slug>/<version>/       the extracted build
//   <data>/games/<slug>/manifest.json    which version is installed + how to run it
//
// An install is download (resumable, URL re-signed per attempt) -> SHA-256
// verify -> extract into <version>.incoming -> confirm the executable ->
// rename into place. An interrupted install therefore never becomes "the"
// installed version. Cancelling keeps the .part so the next try resumes.
// Save data is never touched: games keep it outside this tree.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.hpp"
#include "util.hpp"

namespace vos {

struct Installed {
  std::string slug, version, exePath, launchArgs, sha256;
  fs::path root() const;                 // <games>/<slug>
  fs::path versionDir() const;           // <games>/<slug>/<version>
  fs::path executable() const;           // versionDir / exePath
};

struct InstallJob {
  enum Phase { Queued, Preparing, Downloading, Verifying, Extracting, Finalizing, Done, Failed, Cancelled };
  std::string slug;
  std::atomic<int> phase{Queued};
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false};
  std::thread worker;

  std::string message() const;
  void setMessage(std::string m);
  float fraction() const {
    uint64_t t = total.load();
    return t ? static_cast<float>(done.load()) / static_cast<float>(t) : 0.f;
  }
  const char* phaseLabel() const;
  bool active() const { return phase.load() <= Finalizing; }

 private:
  mutable std::mutex m_;
  std::string message_;
};

class Installer {
 public:
  Installer(Api& api, MainQueue& main) : api_(api), main_(main) {}
  ~Installer();

  // Called on the main thread when a job ends (any outcome).
  std::function<void(const std::string& slug, bool ok, bool cancelled, const std::string& msg)> onFinished;

  void scan();
  const Installed* installed(const std::string& slug) const;
  std::vector<std::pair<std::string, std::string>> installs() const;  // (slug, version)
  std::shared_ptr<InstallJob> job(const std::string& slug) const;

  bool start(const std::string& slug, const std::string& name, const BuildInfo& expected);
  void cancel(const std::string& slug);
  bool uninstall(const std::string& slug);

  void setDemo(bool d) { demo_ = d; }
  // Demo mode only: pretend a version is on disk.
  void injectInstalled(const Installed& i) {
    std::lock_guard<std::mutex> l(m_);
    installed_[i.slug] = i;
  }

 private:
  void runJob(std::shared_ptr<InstallJob> job, BuildInfo expected);
  void runDemo(std::shared_ptr<InstallJob> job, BuildInfo expected);
  void finish(const std::shared_ptr<InstallJob>& job, InstallJob::Phase p, const std::string& msg);

  Api& api_;
  MainQueue& main_;
  bool demo_ = false;
  mutable std::mutex m_;
  std::map<std::string, Installed> installed_;
  std::map<std::string, std::shared_ptr<InstallJob>> jobs_;
};

}  // namespace vos
