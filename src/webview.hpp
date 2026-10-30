// Web titles run in their own browser renderer, separate from the launcher.
//
//  - macOS: a WKWebView in its own window (WebKit's Gamepad API works with the
//    same controllers the launcher uses).
//  - elsewhere: a Chromium-family browser in app/kiosk mode with a dedicated
//    profile, supervised as a child process.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vos {

class WebView {
 public:
  virtual ~WebView() = default;
  // Only ever used for web *games*, which authenticate via the launch ticket
  // in the URL. All other Zonto content is drawn natively by the launcher.
  virtual bool open(const std::string& url, const std::string& title, bool fullscreen, std::string& err) = 0;
  virtual bool isOpen() = 0;
  virtual void close() = 0;
  // True if the renderer ended within seconds of opening without being asked to: a browser that
  // handed the page to an already-running instance and exited, so the window outlives our handle.
  virtual bool endedEarly() const { return false; }
  virtual const char* name() const = 0;
  // Receives JSON posted by the game through window.ZontoLauncher (macOS renderer).
  virtual void setMessageHandler(std::function<void(const std::string&)>) {}
};

std::unique_ptr<WebView> createWebView();

#ifdef __APPLE__
std::unique_ptr<WebView> createNativeWebView();  // webview_mac.mm
#endif

}  // namespace vos
