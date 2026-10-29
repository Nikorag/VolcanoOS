// macOS overlay: a borderless, transparent, click-through NSWindow that floats
// above other apps (including ones in their own fullscreen Space) without ever
// taking focus, showing a bitmap the launcher renders.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <memory>

#include "overlay.hpp"

namespace vos {

namespace {

class MacOverlay : public OverlayWindow {
 public:
  MacOverlay() {
    @autoreleasepool {
      NSRect frame = [NSScreen mainScreen].frame;
      win_ = [[NSWindow alloc] initWithContentRect:frame
                                         styleMask:NSWindowStyleMaskBorderless
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
      win_.releasedWhenClosed = NO;
      win_.opaque = NO;
      win_.backgroundColor = [NSColor clearColor];
      win_.hasShadow = NO;
      win_.ignoresMouseEvents = YES;  // clicks fall through to the game
      win_.level = NSScreenSaverWindowLevel;
      win_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                                NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
      NSView* v = [[NSView alloc] initWithFrame:frame];
      v.wantsLayer = YES;
      v.layer.contentsGravity = kCAGravityResize;
      v.layer.magnificationFilter = kCAFilterLinear;
      win_.contentView = v;
    }
  }

  void present(const void* pixels, int w, int h, int pitch) override {
    @autoreleasepool {
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CFDataRef data = CFDataCreate(nullptr, static_cast<const UInt8*>(pixels), static_cast<CFIndex>(pitch) * h);
      CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
      // ARGB8888 on little-endian = BGRA in memory.
      CGImageRef img = CGImageCreate(w, h, 8, 32, pitch, cs, kCGBitmapByteOrder32Little | kCGImageAlphaFirst, dp, nullptr,
                                     false, kCGRenderingIntentDefault);
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      win_.contentView.layer.contents = (__bridge id)img;
      [CATransaction commit];
      CGImageRelease(img);
      CGDataProviderRelease(dp);
      CFRelease(data);
      CGColorSpaceRelease(cs);
    }
  }

  void show(bool on) override {
    @autoreleasepool {
      if (on == visible_) return;
      visible_ = on;
      if (on) {
        [win_ setFrame:[NSScreen mainScreen].frame display:NO];
        [win_ orderFrontRegardless];  // never makeKey: the game keeps focus
      } else {
        [win_ orderOut:nil];
      }
    }
  }

  bool visible() const override { return visible_; }

 private:
  NSWindow* win_ = nil;
  bool visible_ = false;
};

}  // namespace

std::unique_ptr<OverlayWindow> createOverlayWindow() { return std::make_unique<MacOverlay>(); }

}  // namespace vos
