// macOS web renderer: a WKWebView in its own NSWindow.
//
// Runs on the main thread alongside SDL (SDL's event pump drives NSApp, so the
// two windows coexist). Close is driven from the launcher — the Home chord on
// the controller — or Cmd+W / the title bar.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "webview.hpp"

@interface VOSWebController : NSObject <WKNavigationDelegate, WKUIDelegate, NSWindowDelegate, WKScriptMessageHandler>
@property(strong) NSWindow* window;
@property(strong) WKWebView* web;
@property(strong) id keyMonitor;
@property(assign) BOOL isOpen;
@property(copy) void (^onMessage)(NSString*);
@end

@implementation VOSWebController

- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)message {
  if (self.onMessage && [message.body isKindOfClass:[NSString class]]) self.onMessage(message.body);
}

- (void)teardown {
  if (self.keyMonitor) {
    [NSEvent removeMonitor:self.keyMonitor];
    self.keyMonitor = nil;
  }
  self.isOpen = NO;
}

- (void)windowWillClose:(NSNotification*)note {
  [self teardown];
  // Stop audio/video immediately; the page would otherwise keep playing until
  // the view is deallocated.
  [self.web loadHTMLString:@"" baseURL:nil];
}

// target=_blank / window.open: stay in the one window.
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
  if (action.targetFrame == nil && action.request.URL) {
    [webView loadRequest:action.request];
    handler(WKNavigationActionPolicyCancel);
    return;
  }
  handler(WKNavigationActionPolicyAllow);
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
  if (action.request.URL) [webView loadRequest:action.request];
  return nil;
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
  NSAlert* a = [NSAlert new];
  a.messageText = message;
  [a runModal];
  completionHandler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
  NSAlert* a = [NSAlert new];
  a.messageText = message;
  [a addButtonWithTitle:@"OK"];
  [a addButtonWithTitle:@"Cancel"];
  completionHandler([a runModal] == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:(void (^)(WKPermissionDecision))decisionHandler API_AVAILABLE(macos(12.0)) {
  decisionHandler(WKPermissionDecisionPrompt);
}

@end

namespace vos {

namespace {

// Gives the page a tiny, text-only API to talk to the launcher overlay:
//   ZontoLauncher.notify({title, body}), .achievement(name, description), .setStatus(text)
NSString* bridgeScript() {
  return @"(function(){if(window.ZontoLauncher)return;"
          "function post(o){try{window.webkit.messageHandlers.zonto.postMessage(JSON.stringify(o));}catch(e){}}"
          "window.ZontoLauncher={available:true,version:'" VOS_VERSION "',"
          "notify:function(o){o=o||{};post({kind:'toast',title:o.title,body:o.body});},"
          "achievement:function(n,d){post({kind:'achievement',title:n,body:d||''});},"
          "setStatus:function(t){post({kind:'status',body:String(t||'')});}};"
          "try{window.dispatchEvent(new Event('zontolauncher'));}catch(e){}})();";
}

class MacWebView : public WebView {
 public:
  bool open(const std::string& url, const std::string& title, bool fullscreen, std::string& err) override {
    @autoreleasepool {
      if (isOpen()) {
        [ctl_.window makeKeyAndOrderFront:nil];
        return true;
      }
      NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
      if (!nsurl) {
        err = "the play URL is not usable";
        return false;
      }
      NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];

      WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
      cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
      cfg.applicationNameForUserAgent = @"VolcanoOS/" VOS_VERSION;
      [cfg.preferences setValue:@YES forKey:@"fullScreenEnabled"];
      WKUserScript* script = [[WKUserScript alloc] initWithSource:bridgeScript()
                                                    injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                 forMainFrameOnly:YES];
      [cfg.userContentController addUserScript:script];

      ctl_ = [VOSWebController new];
      ctl_.onMessage = onMessage_;
      [cfg.userContentController addScriptMessageHandler:ctl_ name:@"zonto"];
      NSRect frame = NSMakeRect(0, 0, 1280, 800);
      NSWindow* win = [[NSWindow alloc]
          initWithContentRect:frame
                    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable)
                      backing:NSBackingStoreBuffered
                        defer:NO];
      win.title = [NSString stringWithFormat:@"ZONTO · %@", [nsTitle uppercaseString]];
      win.releasedWhenClosed = NO;
      win.delegate = ctl_;
      win.backgroundColor = [NSColor colorWithSRGBRed:0.043 green:0.035 blue:0.031 alpha:1];
      win.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
      [win center];

      WKWebView* web = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
      web.navigationDelegate = ctl_;
      web.UIDelegate = ctl_;
      web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [web setValue:@NO forKey:@"drawsBackground"];
      win.contentView = web;
      ctl_.window = win;
      ctl_.web = web;
      ctl_.isOpen = YES;

      VOSWebController* ctl = ctl_;
      ctl_.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                              handler:^NSEvent*(NSEvent* e) {
                                                                if ((e.modifierFlags & NSEventModifierFlagCommand) &&
                                                                    [e.charactersIgnoringModifiers isEqualToString:@"w"] &&
                                                                    e.window == ctl.window) {
                                                                  [ctl.window close];
                                                                  return nil;
                                                                }
                                                                return e;
                                                              }];

      [web loadRequest:[NSURLRequest requestWithURL:nsurl]];

      [NSApp activateIgnoringOtherApps:YES];
      [win makeKeyAndOrderFront:nil];
      if (fullscreen) [win toggleFullScreen:nil];
    }
    return true;
  }

  bool isOpen() override { return ctl_ && ctl_.isOpen; }

  void close() override {
    @autoreleasepool {
      if (ctl_ && ctl_.isOpen) [ctl_.window close];
    }
  }

  const char* name() const override { return "wkwebview"; }

  void setMessageHandler(std::function<void(const std::string&)> h) override {
    auto shared = std::make_shared<std::function<void(const std::string&)>>(std::move(h));
    onMessage_ = ^(NSString* body) {
      (*shared)(std::string([body UTF8String] ?: ""));
    };
  }

 private:
  VOSWebController* ctl_ = nil;
  void (^onMessage_)(NSString*) = nil;
};

}  // namespace

std::unique_ptr<WebView> createNativeWebView() { return std::make_unique<MacWebView>(); }

}  // namespace vos
