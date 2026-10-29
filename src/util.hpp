// Small shared helpers: paths, formatting, randomness, the main-thread queue
// and the worker pool every network/disk task runs on.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace vos {

namespace fs = std::filesystem;

// ---- logging ---------------------------------------------------------------
void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ---- paths -----------------------------------------------------------------
// Per-user, machine-local data (never roaming: games are gigabytes).
const fs::path& dataDir();
// Where bundled assets (fonts) live. Falls back to the source tree.
const fs::path& assetDir();
fs::path gamesDir();

// ---- strings ---------------------------------------------------------------
std::string upper(std::string s);
std::string trim(const std::string& s);
std::string humanBytes(uint64_t bytes);
std::string humanPlaytime(int minutes);
std::string urlEncode(const std::string& s);
std::string base64Url(const uint8_t* data, size_t n);
std::string slugify(const std::string& s);
bool endsWith(const std::string& s, const std::string& suffix);
bool readFile(const fs::path& p, std::string& out);
bool writeFileAtomic(const fs::path& p, const std::string& data);

// ---- randomness / time -----------------------------------------------------
bool randomBytes(uint8_t* out, size_t n);
std::string randomToken(size_t bytes);
double nowSeconds();  // monotonic

// ---- threading -------------------------------------------------------------
// Work finishes on a worker; its completion runs on the main (UI) thread.
class MainQueue {
 public:
  void post(std::function<void()> fn);
  void drain();

 private:
  std::mutex m_;
  std::queue<std::function<void()>> q_;
};

class Pool {
 public:
  explicit Pool(unsigned threads = 4);
  ~Pool();
  void submit(std::function<void()> job);

  // Run `work` on a worker, then `done(result)` on the main thread.
  template <class R, class W, class D>
  void run(MainQueue& main, W work, D done) {
    submit([&main, work = std::move(work), done = std::move(done)]() mutable {
      R r = work();
      main.post([done = std::move(done), r = std::move(r)]() mutable { done(std::move(r)); });
    });
  }

 private:
  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  bool stop_ = false;
};

}  // namespace vos
