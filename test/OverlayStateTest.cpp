// license:GPLv3+

// The overlay's state rules and shade table, both taken from the probe firmware. Expected shades are
// worked by hand from that table, and the hardwareGen values are copied from PinMAME's
// src/wpc/gen.h, not taken from the header under test.

#include "OverlayState.h"

#include "Check.h"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace Scorbit;
using Overlay::Platform;
using Clock = Overlay::State::Clock;
using std::chrono::milliseconds;

namespace
{

bool Near(float a, float b)
{
   return std::fabs(a - b) < 1e-6f;
}

std::vector<uint8_t> Payload(uint8_t w, uint8_t h, uint8_t fill, size_t extra = 0)
{
   std::vector<uint8_t> p { w, h };
   p.resize(2 + static_cast<size_t>(w) * h + extra, fill);
   return p;
}

void TestPlatformFromHardwareGen()
{
   const struct
   {
      uint64_t gen;
      Platform expected;
      const char* name;
   } cases[] = {
      { 0x0000000000004ull, Platform::Wpc, "GEN_WPCDMD" },
      { 0x0000000000080ull, Platform::Wpc, "GEN_WPC95" },
      { 0x0000000002000ull, Platform::DataEast, "GEN_DEDMD16" },
      { 0x0000000004000ull, Platform::DataEast, "GEN_DEDMD32" },
      { 0x0000000008000ull, Platform::DataEast, "GEN_DEDMD64 (Sega 192x64)" },
      { 0x0004000000000ull, Platform::DataEast, "GEN_WS" },
      { 0x0010000000000ull, Platform::DataEast, "GEN_WS_2" },
      { 0x0020000000000ull, Platform::Gts3, "GEN_GTS3" },
      { 0x0100000000000ull, Platform::Sam, "GEN_SAM" },
      { 0x0200000000000ull, Platform::AlvinG, "GEN_ALVG" },
      { 0x0400000000000ull, Platform::AlvinG, "GEN_ALVG_DMD2" },
      { 0x20000000000000ull, Platform::Capcom, "GEN_CAPCOM" },
      { 0x0000080000000ull, Platform::Unknown, "GEN_S11" },
      { 0x4000000000000ull, Platform::Unknown, "GEN_SPA (Spike 1, not emulated)" },
      { 0, Platform::Unknown, "not running yet" },
   };
   for (const auto& c : cases)
      CHECK_MSG(Overlay::PlatformOf(c.gen) == c.expected, c.name);
}

void TestShadeTable()
{
   // A value's low four bits are a brightness, 0 to 15, shown at the nearest of the platform's
   // shades: round(b * (S - 1) / 15) of S - 1.

   // WPC and the Data East family: 4 shades.
   for (const Platform p : { Platform::Wpc, Platform::DataEast })
   {
      CHECK(Near(Overlay::Luminance(p, 0, false), 0.f));
      CHECK(Near(Overlay::Luminance(p, 5, false), 1.f / 3));
      CHECK(Near(Overlay::Luminance(p, 7, false), 1.f / 3));
      CHECK(Near(Overlay::Luminance(p, 8, false), 2.f / 3));
      CHECK(Near(Overlay::Luminance(p, 10, false), 2.f / 3));
      CHECK(Near(Overlay::Luminance(p, 15, false), 1.f));
      CHECK_MSG(Near(Overlay::Luminance(p, 0x75, false), 1.f / 3), "bits 4 to 6 are ignored");
   }

   // SAM: 16 shades, so the brightness itself.
   CHECK(Near(Overlay::Luminance(Platform::Sam, 0, true), 0.f));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 8, true), 8.f / 15));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 11, true), 11.f / 15));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 15, true), 1.f));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 0x1f, true), 1.f));

   // Gottlieb System 3: 7 shades.
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 1, false), 0.f));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 3, false), 1.f / 6));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 5, false), 2.f / 6));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 10, false), 4.f / 6));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 15, false), 1.f));

   // Capcom and Alvin G: 5 shades.
   for (const Platform p : { Platform::Capcom, Platform::AlvinG })
   {
      CHECK(Near(Overlay::Luminance(p, 2, true), 0.25f));
      CHECK(Near(Overlay::Luminance(p, 5, true), 0.25f));
      CHECK(Near(Overlay::Luminance(p, 10, true), 0.75f));
      CHECK(Near(Overlay::Luminance(p, 15, true), 1.f));
      CHECK(Near(Overlay::Luminance(p, 0x70, true), 0.f));
   }

   // Unknown: 4 shades on a 4-shade display, 16 on a 16-shade one.
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 10, false), 2.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 5, true), 5.f / 15));
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 15, true), 1.f));

   // A 4-grey ramp (0, 85, 170, 255, brightness 0, 5, 10, 15) rises at every step on WPC, the
   // Data East family and SAM; bit planes swapped WPC's middle two.
   for (const Platform p : { Platform::Wpc, Platform::DataEast, Platform::Sam })
   {
      const bool sixteen = p == Platform::Sam;
      float last = -1.f;
      for (const uint8_t b : { 0, 5, 10, 15 })
      {
         const float l = Overlay::Luminance(p, b, sixteen);
         CHECK_MSG(l > last, Overlay::PlatformName(p) + std::string(": the ramp rises at ") + std::to_string(b));
         last = l;
      }
   }

   // On every platform: brightness never shows darker as it rises, from black to full.
   for (const Platform p : { Platform::Wpc, Platform::DataEast, Platform::Sam, Platform::Gts3, Platform::Capcom,
           Platform::AlvinG, Platform::Unknown })
      for (const bool sixteen : { false, true })
      {
         float last = 0.f;
         for (int b = 0; b <= 15; b++)
         {
            const float l = Overlay::Luminance(p, static_cast<uint8_t>(b), sixteen);
            CHECK_MSG(l >= last, Overlay::PlatformName(p) + std::string(": monotonic at ") + std::to_string(b));
            last = l;
         }
         CHECK(Near(Overlay::Luminance(p, 0, sixteen), 0.f) && Near(Overlay::Luminance(p, 15, sixteen), 1.f));
      }
}

void TestUploadIgnoresTrailingBytes()
{
   Overlay::State s;
   const std::vector<uint8_t> p = Payload(10, 10, 0x03, 7);
   CHECK(s.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
   CHECK(s.Width() == 10 && s.Height() == 10);
   CHECK_MSG(s.Pixels().size() == 100, "pixels " + std::to_string(s.Pixels().size()));
}

void TestShortUploadIsMalformed()
{
   Overlay::State s;
   const std::vector<uint8_t> p = Payload(10, 10, 0x03);
   CHECK(s.Store(p.data(), p.size() - 1) == Overlay::State::Upload::Malformed);
   CHECK(s.Store(p.data(), 1) == Overlay::State::Upload::Malformed);
   CHECK(s.Store(nullptr, 0) == Overlay::State::Upload::Malformed);
   CHECK(s.Pixels().empty());
}

void TestOutOfRangeKeepsThePreviousBitmap()
{
   Overlay::State s;
   const std::vector<uint8_t> first = Payload(10, 10, 0x01);
   CHECK(s.Store(first.data(), first.size()) == Overlay::State::Upload::Stored);
   const uint8_t sizes[][2] = { { 9, 10 }, { 10, 9 }, { 193, 10 }, { 10, 65 } };
   for (const auto& wh : sizes)
   {
      const std::vector<uint8_t> p = Payload(wh[0], wh[1], 0x02);
      CHECK_MSG(s.Store(p.data(), p.size()) == Overlay::State::Upload::Ignored,
         std::to_string(wh[0]) + "x" + std::to_string(wh[1]));
   }
   CHECK(s.Width() == 10 && s.Height() == 10);
   CHECK(s.Pixels() == std::vector<uint8_t>(100, 0x01));

   const std::vector<uint8_t> largest = Payload(192, 64, 0x02);
   CHECK(s.Store(largest.data(), largest.size()) == Overlay::State::Upload::Stored);
}

void TestControlArmsBeforeAnyUpload()
{
   Overlay::State s;
   s.SetTransition({ .on = false });   // the timer rules alone; the sweep is in OverlayTransitionTest
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 1000, t0);
   CHECK_MSG(!s.Visible(t0, 128), "nothing to show yet");

   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   CHECK_MSG(s.Visible(t0 + milliseconds(500), 128), "an upload inside the window shows at once");
   CHECK_MSG(!s.Visible(t0 + milliseconds(1000), 128), "hidden at the deadline");
}

void TestUploadAfterExpiryStaysHidden()
{
   Overlay::State s;
   s.SetTransition({ .on = false });   // the timer rules alone; the sweep is in OverlayTransitionTest
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 100, t0);
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   CHECK(!s.Visible(t0 + milliseconds(200), 128));
}

void TestTimerRestartsAndUntilHidden()
{
   Overlay::State s;
   s.SetTransition({ .on = false });   // the timer rules alone; the sweep is in OverlayTransitionTest
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 100, t0);
   s.Control(true, 100, t0 + milliseconds(90));
   CHECK_MSG(s.Visible(t0 + milliseconds(150), 128), "control restarts the timer");
   CHECK(!s.Visible(t0 + milliseconds(190), 128));

   s.Control(true, 0, t0);
   CHECK_MSG(s.Visible(t0 + std::chrono::hours(1), 128), "0 shows until hidden");
   s.Control(false, 500, t0);
   CHECK_MSG(!s.Visible(t0, 128), "hide ignores its duration");
}

void TestUploadWhileShownReplaces()
{
   Overlay::State s;
   s.SetTransition({ .on = false });   // the timer rules alone; the sweep is in OverlayTransitionTest
   const std::vector<uint8_t> a = Payload(10, 10, 0x01), b = Payload(12, 10, 0x02);
   s.Store(a.data(), a.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 0, t0);
   s.Store(b.data(), b.size());
   CHECK(s.Visible(t0, 128));
   CHECK(s.Width() == 12 && s.Pixels() == std::vector<uint8_t>(120, 0x02));
}

void TestClearDropsBitmapAndTimer()
{
   Overlay::State s;
   s.SetTransition({ .on = false });   // the timer rules alone; the sweep is in OverlayTransitionTest
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 0, t0);
   s.Clear();
   CHECK(!s.Visible(t0, 128));
   CHECK(s.Pixels().empty() && s.Width() == 0 && s.Height() == 0);
   // A new session starts with no overlay armed: an upload alone shows nothing.
   s.Store(p.data(), p.size());
   CHECK(!s.Visible(t0, 128));
}

}

int main()
{
   TestPlatformFromHardwareGen();
   TestShadeTable();
   TestUploadIgnoresTrailingBytes();
   TestShortUploadIsMalformed();
   TestOutOfRangeKeepsThePreviousBitmap();
   TestControlArmsBeforeAnyUpload();
   TestUploadAfterExpiryStaysHidden();
   TestTimerRestartsAndUntilHidden();
   TestUploadWhileShownReplaces();
   TestClearDropsBitmapAndTimer();
   return ScorbitTest::Summary("overlay_state_test");
}
