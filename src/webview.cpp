#include "webview.hpp"

#include <cstdlib>

#include "runner.hpp"
#include "util.hpp"

namespace vos {

namespace {

// Fallback renderer: a Chromium-family browser in app mode. It gets its own
// profile so it never touches the player's everyday browser, and it's a real
// separate process, so a misbehaving game can't take the launcher down.
class BrowserAppWebView : public WebView {
 public:
  bool open(const std::string& url, const std::string& title, bool fullscreen, std::string& err) override {
    fs::path exe = findBrowser();
    if (exe.empty()) {
      err = "No Chromium-based browser (Chrome, Edge, Chromium, Brave) was found to run web titles.";
      return false;
    }
    fs::path profile = dataDir() / "web-profile";
    std::string args = "--app=" + url + " --user-data-dir=" + profile.string() +
                       " --no-first-run --no-default-browser-check --autoplay-policy=no-user-gesture-required";
    if (fullscreen) args += " --start-fullscreen";
    (void)title;
    // The URL carries a launch ticket; args are whitespace-split, which is
    // fine because Zonto play URLs never contain spaces.
    return proc_.spawn(exe, dataDir(), args, dataDir() / "logs" / "web.log", err);
  }
  bool isOpen() override {
    int code;
    if (proc_.running() && proc_.poll(code)) return false;
    return proc_.running();
  }
  void close() override { proc_.terminate(); }
  const char* name() const override { return "browser-app"; }

 private:
  static fs::path findBrowser() {
    std::vector<std::string> candidates;
#if defined(_WIN32)
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
      const char* base = std::getenv(env);
      if (!base) continue;
      candidates.push_back(std::string(base) + "\\Google\\Chrome\\Application\\chrome.exe");
      candidates.push_back(std::string(base) + "\\Microsoft\\Edge\\Application\\msedge.exe");
      candidates.push_back(std::string(base) + "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe");
    }
#elif defined(__APPLE__)
    candidates = {"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Chromium.app/Contents/MacOS/Chromium",
                  "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                  "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"};
#else
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/usr/local/bin";
    size_t i = 0;
    while (i <= p.size()) {
      size_t j = p.find(':', i);
      std::string dir = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
      for (const char* n : {"google-chrome-stable", "google-chrome", "chromium", "chromium-browser", "microsoft-edge",
                            "brave-browser"})
        candidates.push_back(dir + "/" + n);
      if (j == std::string::npos) break;
      i = j + 1;
    }
#endif
    std::error_code ec;
    for (auto& c : candidates)
      if (fs::exists(c, ec)) return fs::path(c);
    return {};
  }

  GameProcess proc_;
};

}  // namespace

std::unique_ptr<WebView> createWebView() {
#ifdef __APPLE__
  return createNativeWebView();
#else
  return std::make_unique<BrowserAppWebView>();
#endif
}

}  // namespace vos
