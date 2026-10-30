// license:GPLv3+

#include "StatusWindow.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

// A plain NSPanel, never a subclass: VPX unloads this dylib while AppKit can still reach the
// window, and an object whose class lived here then crashes AppKit's window cache at quit.
// A borderless panel already refuses key and main, which keeps the playfield focused.
namespace Scorbit
{

static constexpr CGFloat SIZE = 28;
static constexpr CGFloat INSET = 16;
static constexpr NSTimeInterval FAKE_STATE_EVERY = 4;

static NSPanel* panel = nil;
static NSTimer* fakeTimer = nil;
static int fakeState = 0;
static bool wanted = false;
static id activeObserver = nil;
static id resignObserver = nil;

// The playfield is the largest visible window VPX owns; its screen is where the light goes.
static NSScreen* PlayfieldScreen()
{
   NSWindow* best = nil;
   for (NSWindow* w in [NSApp windows])
      if (w != panel && [w isVisible] && (best == nil || w.frame.size.width * w.frame.size.height > best.frame.size.width * best.frame.size.height))
         best = w;
   return (best != nil && best.screen != nil) ? best.screen : [NSScreen mainScreen];
}

// The prototype's stand-in for the daemon's Status: steady green, then amber, red and blue pulsing.
static void ApplyFakeState()
{
   static const struct { CGFloat r, g, b; bool pulse; } states[] = {
      { 0.20, 0.80, 0.30, false }, { 1.00, 0.70, 0.10, true }, { 0.90, 0.20, 0.20, true }, { 0.20, 0.50, 1.00, true },
   };
   const auto& s = states[fakeState++ % 4];
   CALayer* layer = panel.contentView.layer;
   layer.backgroundColor = [NSColor colorWithSRGBRed:s.r green:s.g blue:s.b alpha:1].CGColor;
   [layer removeAllAnimations];
   if (s.pulse)
   {
      // An OS-side animation, so the light costs VPX's render path nothing. Its strings are heap
      // copies: Core Animation keeps them past the dylib's unload, and a literal lives in the dylib.
      CABasicAnimation* pulse = [CABasicAnimation animationWithKeyPath:[NSString stringWithUTF8String:"opacity"]];
      pulse.fromValue = [NSNumber numberWithDouble:1.0];
      pulse.toValue = [NSNumber numberWithDouble:0.25];
      pulse.duration = 0.6;
      pulse.autoreverses = YES;
      pulse.repeatCount = HUGE_VALF;
      [layer addAnimation:pulse forKey:[NSString stringWithUTF8String:"pulse"]];
   }
}

void ShowStatusWindow(int corner)
{
   if (corner < 1 || corner > 4)
   {
      HideStatusWindow();
      return;
   }
   if (panel == nil)
   {
      panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, SIZE, SIZE)
                                                    styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
      panel.opaque = NO;
      panel.backgroundColor = [NSColor clearColor];
      panel.hasShadow = NO;
      panel.ignoresMouseEvents = YES;
      // Not hidesOnDeactivate: a panel first ordered front while VPX is still inactive (the
      // game starts before VPX activates) is never brought back by it. Follow activation instead.
      panel.hidesOnDeactivate = NO;
      panel.releasedWhenClosed = NO;
      panel.level = NSStatusWindowLevel;
      // VPX's fullscreen is its own Space; only a full-screen-auxiliary window shows there.
      panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary
         | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
      panel.contentView.wantsLayer = YES;
      panel.contentView.layer.cornerRadius = SIZE / 2;

      // queue:nil delivers on the posting thread, which for these is the main thread.
      NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
      activeObserver = [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                                       usingBlock:^(NSNotification*) { if (wanted) [panel orderFrontRegardless]; }];
      resignObserver = [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                                       usingBlock:^(NSNotification*) { [panel orderOut:nil]; }];
   }

   const NSRect area = PlayfieldScreen().frame;
   const CGFloat x = (corner == 1 || corner == 3) ? NSMinX(area) + INSET : NSMaxX(area) - INSET - SIZE;
   const CGFloat y = (corner == 3 || corner == 4) ? NSMinY(area) + INSET : NSMaxY(area) - INSET - SIZE;
   [panel setFrameOrigin:NSMakePoint(x, y)];

   ApplyFakeState();
   if (fakeTimer == nil)
      fakeTimer = [NSTimer scheduledTimerWithTimeInterval:FAKE_STATE_EVERY repeats:YES block:^(NSTimer*) { ApplyFakeState(); }];
   wanted = true;
   if ([NSApp isActive])
      [panel orderFrontRegardless];
}

void HideStatusWindow()
{
   wanted = false;
   // Released here, not by a later pool drain: the timer's and observers' blocks are code in
   // this dylib, which VPX unloads right after.
   @autoreleasepool
   {
      [fakeTimer invalidate];
      fakeTimer = nil;
      if (activeObserver != nil)
      {
         [[NSNotificationCenter defaultCenter] removeObserver:activeObserver];
         [[NSNotificationCenter defaultCenter] removeObserver:resignObserver];
         activeObserver = resignObserver = nil;
      }
      [panel.contentView.layer removeAllAnimations];
      [panel orderOut:nil];
      [panel close];
      panel = nil;
   }
   // Commit now: the run loop's own commit comes after VPX has unloaded the dylib.
   [CATransaction flush];
}

}
