// license:GPLv3+

#include "StatusWindow.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>

// A plain NSPanel, never a subclass: VPX unloads this dylib while AppKit can still reach the
// window, and an object whose class lived here then crashes AppKit's window cache at quit.
// A borderless panel already refuses key and main, which keeps the playfield focused.
// Nothing handed to Core Animation is a literal of this dylib either (docs/status-window.md).
namespace Scorbit
{

static constexpr CGFloat SIZE = 44;
static constexpr CGFloat INSET = 16;
static constexpr CGFloat RING_RADIUS = 18.5;
static constexpr CGFloat SEGMENT = 3;
static constexpr CGFloat DOT = 9;
static constexpr CGFloat DOT_OFFSET = 10;
static constexpr double RING_PULSE_S = 1.5;
static constexpr double FLASH_S = 0.5;

static NSPanel* panel = nil;
static CALayer* ring = nil;
static NSMutableArray<CALayer*>* segments = nil;
static CALayer* systemDot = nil;
static CALayer* sessionDot = nil;
static CATextLayer* glyph = nil;
static CAShapeLayer* flashRing = nil;
static LightView shown;
static int shownCorner = 0;
static id activeObserver = nil;
static id resignObserver = nil;

// A heap copy: Core Animation keeps keys and key paths past the dylib's unload.
static NSString* Heap(const char* s)
{
   return [NSString stringWithUTF8String:s];
}

static CGColorRef Color(LightRgb rgb)
{
   return [NSColor colorWithSRGBRed:rgb.r / 255.0 green:rgb.g / 255.0 blue:rgb.b / 255.0 alpha:1].CGColor;
}

// The playfield's screen. VPX plays only while the playfield holds keyboard focus, and on macOS it
// makes the backglass, score view and topper unfocusable, so while VPX is active its key window
// is the playfield. Before that, the main screen; Place runs again on activation and every tick.
static NSScreen* PlayfieldScreen()
{
   NSWindow* key = [NSApp keyWindow];
   return (key != nil && key != panel && key.screen != nil) ? key.screen : [NSScreen mainScreen];
}

static void Place(int corner)
{
   const NSRect area = PlayfieldScreen().frame;
   const CGFloat x = (corner == 1 || corner == 3) ? NSMinX(area) + INSET : NSMaxX(area) - INSET - SIZE;
   const CGFloat y = (corner == 3 || corner == 4) ? NSMinY(area) + INSET : NSMaxY(area) - INSET - SIZE;
   if (!NSEqualPoints(panel.frame.origin, NSMakePoint(x, y)))
      [panel setFrameOrigin:NSMakePoint(x, y)];
}

static CALayer* Disc(CALayer* parent, CGFloat diameter, CGPoint centre)
{
   CALayer* layer = [CALayer layer];
   layer.bounds = CGRectMake(0, 0, diameter, diameter);
   layer.position = centre;
   layer.cornerRadius = diameter / 2;
   [parent addSublayer:layer];
   return layer;
}

static void Build()
{
   CALayer* root = panel.contentView.layer;
   root.cornerRadius = SIZE / 2;
   // Dark behind the light, so its colours read over a bright playfield.
   root.backgroundColor = [NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:0.6].CGColor;
   const CGPoint c = CGPointMake(SIZE / 2, SIZE / 2);

   ring = [CALayer layer];
   ring.frame = CGRectMake(0, 0, SIZE, SIZE);
   [root addSublayer:ring];
   segments = [NSMutableArray arrayWithCapacity:Light::RING_SEGMENTS];
   for (size_t i = 0; i < Light::RING_SEGMENTS; i++)
   {
      // From the top, clockwise, like the pad's LED 0.
      const double a = M_PI / 2 - 2 * M_PI * static_cast<double>(i) / Light::RING_SEGMENTS;
      [segments addObject:Disc(ring, SEGMENT, CGPointMake(c.x + RING_RADIUS * std::cos(a), c.y + RING_RADIUS * std::sin(a)))];
   }

   flashRing = [CAShapeLayer layer];
   flashRing.frame = CGRectMake(0, 0, SIZE, SIZE);
   CGPathRef circle = CGPathCreateWithEllipseInRect(CGRectMake(c.x - RING_RADIUS, c.y - RING_RADIUS, 2 * RING_RADIUS, 2 * RING_RADIUS), nullptr);
   flashRing.path = circle;
   CGPathRelease(circle);
   flashRing.fillColor = nil;
   flashRing.strokeColor = [NSColor whiteColor].CGColor;
   flashRing.lineWidth = SEGMENT;
   flashRing.opacity = 0;
   [root addSublayer:flashRing];

   systemDot = Disc(root, DOT, CGPointMake(c.x - DOT_OFFSET, c.y));
   sessionDot = Disc(root, DOT, CGPointMake(c.x + DOT_OFFSET, c.y));

   glyph = [CATextLayer layer];
   glyph.bounds = CGRectMake(0, 0, 8, 14);
   glyph.position = c;
   glyph.font = (__bridge CFTypeRef)[NSFont boldSystemFontOfSize:12];
   glyph.fontSize = 12;
   glyph.alignmentMode = kCAAlignmentCenter;
   glyph.foregroundColor = [NSColor whiteColor].CGColor;
   glyph.contentsScale = PlayfieldScreen().backingScaleFactor;
   [root addSublayer:glyph];
}

static void ShowDot(CALayer* layer, const LightDot& dot)
{
   [layer removeAllAnimations];
   layer.hidden = !dot.lit;
   if (!dot.lit)
      return;
   layer.backgroundColor = Color(dot.rgb);
   // The device's blink, run by the window server, so it costs VPX's render path nothing.
   const double period = (dot.onMs + dot.offMs) / 1000.0;
   CAKeyframeAnimation* blink = [CAKeyframeAnimation animationWithKeyPath:Heap("opacity")];
   blink.values = [NSArray arrayWithObjects:[NSNumber numberWithDouble:1], [NSNumber numberWithDouble:0], nil];
   blink.keyTimes = [NSArray arrayWithObjects:[NSNumber numberWithDouble:0], [NSNumber numberWithDouble:dot.onMs / 1000.0 / period],
      [NSNumber numberWithDouble:1], nil];
   blink.calculationMode = kCAAnimationDiscrete;
   blink.duration = period;
   blink.repeatCount = HUGE_VALF;
   [layer addAnimation:blink forKey:Heap("blink")];
}

static void ShowRing(const LightView& view)
{
   [ring removeAllAnimations];
   ring.hidden = view.ring == LightRing::Dark;
   if (ring.hidden)
      return;
   for (size_t i = 0; i < Light::RING_SEGMENTS; i++)
      segments[i].backgroundColor = view.ring == LightRing::Solid ? Color(view.ringRgb)
         : [NSColor colorWithHue:static_cast<double>(i) / Light::RING_SEGMENTS saturation:1 brightness:1 alpha:1].CGColor;
   if (view.ring != LightRing::Rainbow)
      return;
   CABasicAnimation* pulse = [CABasicAnimation animationWithKeyPath:Heap("opacity")];
   pulse.fromValue = [NSNumber numberWithDouble:1];
   pulse.toValue = [NSNumber numberWithDouble:0.35];
   pulse.duration = RING_PULSE_S;
   pulse.autoreverses = YES;
   pulse.repeatCount = HUGE_VALF;
   [ring addAnimation:pulse forKey:Heap("pulse")];
}

static void Show(const LightView& view)
{
   [CATransaction begin];
   [CATransaction setDisableActions:YES];
   ShowDot(systemDot, view.system);
   ShowDot(sessionDot, view.session);
   ShowRing(view);
   glyph.string = view.glyph == LightGlyph::Problem ? Heap("!") : view.glyph == LightGlyph::Unknown ? Heap("?") : Heap("");
   [CATransaction commit];
}

static void Flash()
{
   CAKeyframeAnimation* flash = [CAKeyframeAnimation animationWithKeyPath:Heap("opacity")];
   flash.values = [NSArray arrayWithObjects:[NSNumber numberWithDouble:0], [NSNumber numberWithDouble:1], [NSNumber numberWithDouble:0], nil];
   flash.keyTimes = [NSArray arrayWithObjects:[NSNumber numberWithDouble:0], [NSNumber numberWithDouble:0.3], [NSNumber numberWithDouble:1], nil];
   flash.duration = FLASH_S;
   [flashRing addAnimation:flash forKey:Heap("flash")];
}

static void RemoveAnimations(CALayer* layer)
{
   [layer removeAllAnimations];
   for (CALayer* sub in layer.sublayers)
      RemoveAnimations(sub);
}

static void Hide()
{
   // Released here, not by a later pool drain: the observers' blocks are code in this dylib,
   // which VPX unloads right after.
   @autoreleasepool
   {
      [[NSNotificationCenter defaultCenter] removeObserver:activeObserver];
      [[NSNotificationCenter defaultCenter] removeObserver:resignObserver];
      activeObserver = resignObserver = nil;
      RemoveAnimations(panel.contentView.layer);
      [panel orderOut:nil];
      [panel close];
      panel = nil;
      ring = systemDot = sessionDot = nil;
      segments = nil;
      glyph = nil;
      flashRing = nil;
   }
   // Commit now: the run loop's own commit comes after VPX has unloaded the dylib.
   [CATransaction flush];
}

bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash)
{
   if (corner < 1 || corner > 4 || !view.visible)
   {
      if (panel != nil)
         Hide();
      shownCorner = 0;
      return false;
   }
   const bool appeared = panel == nil;
   @autoreleasepool
   {
      if (appeared)
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
         Build();

         // queue:nil delivers on the posting thread, which for these is the main thread.
         NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
         activeObserver = [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                                          usingBlock:^(NSNotification*) { Place(shownCorner); [panel orderFrontRegardless]; }];
         resignObserver = [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                                          usingBlock:^(NSNotification*) { [panel orderOut:nil]; }];
      }
      if (appeared || !(view == shown))
      {
         Show(view);
         shown = view;
      }
      if (flash)
         Flash();
      panel.alphaValue = opacity;
   }
   shownCorner = corner;
   Place(corner);
   if (appeared && [NSApp isActive])
      [panel orderFrontRegardless];
   return appeared;
}

}
