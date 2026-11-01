// license:GPLv3+

#pragma once

// The status light drawn in software, for windows with no animation layer of their own (Windows, X11).
// It draws what the macOS panel draws with Core Animation (StatusWindow_mac.mm): the same geometry,
// blink, ring pulse and claim flash, so a frame here matches that panel at the same moment.

#include "StatusLight.h"

#include <cstdint>
#include <vector>

namespace Scorbit
{

namespace LightGeometry
{
// In points; a frame is drawn at DIAMETER times its scale (pixels per point).
inline constexpr double DIAMETER = 44;
inline constexpr double RING_RADIUS = 18.5;
inline constexpr double SEGMENT = 3;
inline constexpr double DOT = 9;
inline constexpr double DOT_OFFSET = 10;
inline constexpr double RING_PULSE_S = 1.5;    // full to 0.35 and back takes twice this
inline constexpr double RING_PULSE_LOW = 0.35;
inline constexpr double FLASH_S = 0.5;
inline constexpr uint8_t BACKGROUND_ALPHA = 153; // black at 0.6 behind the light
}

// What moves, at one moment. Equal phases draw equal frames, so a window redraws only when this
// (or the view, scale or opacity) changes.
struct LightPhase
{
   bool systemLit = false;
   bool sessionLit = false;
   uint8_t ring = 0;    // the ring's opacity
   uint8_t flash = 0;   // the claim flash's opacity
   bool operator==(const LightPhase&) const = default;
};

// seconds: since the view was first shown. flashSeconds: since a claim flash began, or negative for none.
LightPhase PhaseAt(const LightView& view, double seconds, double flashSeconds);

struct LightFrame
{
   int size = 0;                  // pixels, square
   std::vector<uint32_t> pixels;  // premultiplied 0xAARRGGBB, row by row from the top
};

// scale: pixels per point, e.g. 1.5 at 144 dpi.
void Rasterize(const LightView& view, const LightPhase& phase, double scale, LightFrame& out);

}
