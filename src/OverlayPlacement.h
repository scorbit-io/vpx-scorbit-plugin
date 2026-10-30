// license:GPLv3+

#pragma once

// Where an overlay lands on the display, and which sizes are accepted, exactly as the
// DMD probe firmware does it, so overlay code written against the probe behaves the
// same on VPX (SB-5699). Reference: the probe firmware's overlay write and
// frame-to-samples code.
//
// Overlays are normally full display-size frames shaped by transparency, which places
// them at the origin. These rules decide only the mismatched cases: a smaller overlay
// is centred, a larger one is centred and its edges cropped.

#include <algorithm>
#include <cstdint>

namespace Scorbit::Overlay
{

// The firmware ignores an upload outside these bounds and keeps the previous overlay.
inline constexpr uint32_t MIN_SIDE = 10;
inline constexpr uint32_t MAX_WIDTH = 192;
inline constexpr uint32_t MAX_HEIGHT = 64;

inline bool Accepted(uint32_t width, uint32_t height)
{
   return width >= MIN_SIDE && height >= MIN_SIDE && width <= MAX_WIDTH && height <= MAX_HEIGHT;
}

// The visible part of the overlay: `width` x `height` pixels copied from (srcX, srcY)
// in the overlay to (dstX, dstY) on the display. Empty when nothing overlaps.
struct Placement
{
   uint32_t dstX = 0, dstY = 0;
   uint32_t srcX = 0, srcY = 0;
   uint32_t width = 0, height = 0;
};

namespace Detail
{

// One axis. The firmware's origin is (display - overlay) / 2 in signed int, which C and
// C++ both truncate toward zero; a negative origin crops the overlay's leading edge.
inline void Axis(uint32_t display, uint32_t overlay, uint32_t& dst, uint32_t& src, uint32_t& len)
{
   const int64_t origin = (static_cast<int64_t>(display) - static_cast<int64_t>(overlay)) / 2;
   const int64_t first = std::max<int64_t>(origin, 0);
   const int64_t last = std::min<int64_t>(origin + overlay, display); // exclusive
   if (last <= first)
   {
      dst = src = len = 0;
      return;
   }
   dst = static_cast<uint32_t>(first);
   src = static_cast<uint32_t>(first - origin);
   len = static_cast<uint32_t>(last - first);
}

}

inline Placement Place(uint32_t displayWidth, uint32_t displayHeight, uint32_t overlayWidth, uint32_t overlayHeight)
{
   Placement p;
   Detail::Axis(displayWidth, overlayWidth, p.dstX, p.srcX, p.width);
   Detail::Axis(displayHeight, overlayHeight, p.dstY, p.srcY, p.height);
   if (p.width == 0 || p.height == 0)
      p = { };
   return p;
}

}
