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
    if (proc_.running()) {  // never spawn a second window over the first
      err = "A web game is already open.";
      return false;
    }
    // Always a dedicated profile, never the player's own: a browser that finds a running
    // instance on the same profile hands the URL to it and exits at once, which looks to us
    // like the game closing while its window is still on screen. A Flatpak browser can't see
    // our data dir, so its profile lives inside its own sandbox data dir.
    fs::path profile = dataDir() / "web-profile";
    const std::string exeStr = exe.string();
    if (exeStr.find("/flatpak/exports/") != std::string::npos) {
      const char* home = std::getenv("HOME");
      if (home) profile = fs::path(home) / ".var" / "app" / exe.filename() / "data" / "zonto-web-profile";
    }
    std::string args = "--app=" + url + " --user-data-dir=" + profile.string() +
                       " --no-first-run --no-default-browser-check --autoplay-policy=no-user-gesture-required";
    if (fullscreen) args += " --start-fullscreen";
    (void)title;
    openedAt_ = nowSeconds();
    closeAt_ = 0;
    endedEarly_ = false;
    // The URL carries a launch ticket; args are whitespace-split, which is
    // fine because Zonto play URLs never contain spaces.
    return proc_.spawn(exe, dataDir(), args, dataDir() / "logs" / "web.log", err);
  }
  bool isOpen() override {
    int code;
    if (proc_.running() && proc_.poll(code)) {
      const double up = nowSeconds() - openedAt_;
      endedEarly_ = closeAt_ == 0 && up < 8;  // we didn't ask it to close, and it left almost immediately
      logf("browser process ended after %.1fs (exit %d)%s", up, code,
           endedEarly_ ? ": it probably handed the page to an already-running browser" : "");
      return false;
    }
    // Asked to close but still running: it's ignoring SIGTERM, so stop asking nicely.
    if (proc_.running() && closeAt_ > 0 && nowSeconds() - closeAt_ > 3) {
      logf("browser ignored the close request; killing it");
      proc_.forceKill();
      closeAt_ = nowSeconds();
    }
    return proc_.running();
  }
  void close() override {
    if (!proc_.running() || closeAt_ > 0) return;
    proc_.terminate();
    closeAt_ = nowSeconds();
  }
  bool endedEarly() const override { return endedEarly_; }
  const char* name() const override { return "browser-app"; }

 private:
  double openedAt_ = 0, closeAt_ = 0;
  bool endedEarly_ = false;

  static fs::path findBrowser() {
    // Explicit override (also how the tests substitute a fake browser).
    if (const char* o = std::getenv("VOS_BROWSER"); o && *o) return fs::path(o);
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
    // Flatpak exports (not on PATH): the common way to get Chromium on a Steam Deck.
    const char* home = std::getenv("HOME");
    for (const char* id : {"org.chromium.Chromium", "com.google.Chrome", "com.brave.Browser", "com.microsoft.Edge"}) {
      candidates.push_back(std::string("/var/lib/flatpak/exports/bin/") + id);
      if (home) candidates.push_back(std::string(home) + "/.local/share/flatpak/exports/bin/" + id);
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
  const char* force = std::getenv("VOS_WEBVIEW");  // "browser" = use the Chromium path (for testing)
  if (!(force && std::string(force) == "browser")) return createNativeWebView();
  return std::make_unique<BrowserAppWebView>();
#else
  return std::make_unique<BrowserAppWebView>();
#endif
}

}  // namespace vos
