// license:GPLv3+

// Overlay placement and size rules against the DMD probe firmware (its overlay
// write and frame-to-samples code). Expected values are worked from the firmware's
// own arithmetic, not from the helper, on the three hardware displays: 128x32, 128x16
// and 192x64.

#include "OverlayPlacement.h"

#include "Check.h"

#include <string>

using namespace Scorbit;

namespace
{

// The firmware's mapping, one display pixel at a time: x0 = (W - w) / 2 in int, and a
// display pixel (x, y) shows overlay pixel (x - x0, y - y0) when inside it. Returns the
// overlay pixel a display pixel shows, or -1.
long FirmwarePixel(int W, int H, int w, int h, int x, int y)
{
   const int x0 = (W - w) / 2, y0 = (H - h) / 2;
   if (x < x0 || x > x0 + w - 1 || y < y0 || y > y0 + h - 1)
      return -1;
   return static_cast<long>(y - y0) * w + (x - x0);
}

long HelperPixel(const Overlay::Placement& p, int w, int x, int y)
{
   if (x < static_cast<int>(p.dstX) || x >= static_cast<int>(p.dstX + p.width)
      || y < static_cast<int>(p.dstY) || y >= static_cast<int>(p.dstY + p.height))
      return -1;
   return static_cast<long>(p.srcY + (y - p.dstY)) * w + (p.srcX + (x - p.dstX));
}

std::string Name(int W, int H, int w, int h)
{
   return std::to_string(w) + "x" + std::to_string(h) + " on " + std::to_string(W) + "x" + std::to_string(H);
}

void TestFullFrameSitsAtTheOrigin()
{
   const int displays[][2] = { { 128, 32 }, { 128, 16 }, { 192, 64 } };
   for (const auto& d : displays)
   {
      const Overlay::Placement p = Overlay::Place(d[0], d[1], d[0], d[1]);
      CHECK_MSG(p.dstX == 0 && p.dstY == 0 && p.srcX == 0 && p.srcY == 0, Name(d[0], d[1], d[0], d[1]));
      CHECK_MSG(p.width == static_cast<uint32_t>(d[0]) && p.height == static_cast<uint32_t>(d[1]), Name(d[0], d[1], d[0], d[1]));
   }
}

void TestSmallerIsCentred()
{
   // 64x16 on 128x32: origin (32, 8), whole overlay shown.
   Overlay::Placement p = Overlay::Place(128, 32, 64, 16);
   CHECK(p.dstX == 32 && p.dstY == 8 && p.srcX == 0 && p.srcY == 0 && p.width == 64 && p.height == 16);

   // 128x32 on 192x64: origin (32, 16).
   p = Overlay::Place(192, 64, 128, 32);
   CHECK(p.dstX == 32 && p.dstY == 16 && p.width == 128 && p.height == 32);

   // An odd difference truncates toward the leading edge: 127x31 on 128x32 is at (0, 0).
   p = Overlay::Place(128, 32, 127, 31);
   CHECK(p.dstX == 0 && p.dstY == 0 && p.width == 127 && p.height == 31);
}

void TestLargerIsCentredAndCropped()
{
   // 192x64 on 128x32: origin (-32, -16), so the middle 128x32 shows.
   Overlay::Placement p = Overlay::Place(128, 32, 192, 64);
   CHECK(p.dstX == 0 && p.dstY == 0 && p.srcX == 32 && p.srcY == 16 && p.width == 128 && p.height == 32);

   // 128x32 on 128x16: rows 8..23 of the overlay.
   p = Overlay::Place(128, 16, 128, 32);
   CHECK(p.dstX == 0 && p.dstY == 0 && p.srcX == 0 && p.srcY == 8 && p.width == 128 && p.height == 16);

   // An odd overshoot truncates toward zero, as C does: 131 wide on 128 gives origin -1.
   p = Overlay::Place(128, 32, 131, 32);
   CHECK(p.srcX == 1 && p.width == 128);
}

// Every display pixel, every case: the helper picks the same overlay pixel as the
// firmware's own loop.
void TestMatchesTheFirmwarePixelForPixel()
{
   const int displays[][2] = { { 128, 32 }, { 128, 16 }, { 192, 64 } };
   const int overlays[][2] = { { 128, 32 }, { 128, 16 }, { 192, 64 }, { 10, 10 }, { 64, 16 }, { 127, 31 }, { 131, 33 }, { 191, 63 } };
   for (const auto& d : displays)
      for (const auto& o : overlays)
      {
         const Overlay::Placement p = Overlay::Place(d[0], d[1], o[0], o[1]);
         int mismatches = 0;
         for (int y = 0; y < d[1]; y++)
            for (int x = 0; x < d[0]; x++)
               if (FirmwarePixel(d[0], d[1], o[0], o[1], x, y) != HelperPixel(p, o[0], x, y))
                  mismatches++;
         CHECK_MSG(mismatches == 0, Name(d[0], d[1], o[0], o[1]) + ": " + std::to_string(mismatches) + " pixels differ");
      }
}

void TestSizeBoundsMatchTheFirmware()
{
   CHECK(Overlay::Accepted(10, 10));
   CHECK(Overlay::Accepted(192, 64));
   CHECK(Overlay::Accepted(128, 16));
   CHECK_MSG(!Overlay::Accepted(9, 32), "under 10 wide is ignored");
   CHECK_MSG(!Overlay::Accepted(128, 9), "under 10 high is ignored");
   CHECK_MSG(!Overlay::Accepted(193, 32), "over 192 wide is ignored");
   CHECK_MSG(!Overlay::Accepted(128, 65), "over 64 high is ignored");
   CHECK_MSG(!Overlay::Accepted(0, 0), "an empty upload is ignored");
}

void TestNoOverlapIsEmpty()
{
   const Overlay::Placement p = Overlay::Place(0, 0, 128, 32);
   CHECK(p.width == 0 && p.height == 0);
}

}

int main()
{
   TestFullFrameSitsAtTheOrigin();
   TestSmallerIsCentred();
   TestLargerIsCentredAndCropped();
   TestMatchesTheFirmwarePixelForPixel();
   TestSizeBoundsMatchTheFirmware();
   TestNoOverlapIsEmpty();
   return ScorbitTest::Summary("overlay_placement_test");
}
