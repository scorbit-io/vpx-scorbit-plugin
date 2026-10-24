// license:GPLv3+

// The overlay's state rules and shade table against scorbitd design.md, "Message layouts
// (protocol 1.0, slice 4)", which takes both from the probe firmware. Expected shades are
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
   // WPC: popcount(v & 7) of 3.
   CHECK(Near(Overlay::Luminance(Platform::Wpc, 0, false), 0.f));
   CHECK(Near(Overlay::Luminance(Platform::Wpc, 1, false), 1.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::Wpc, 4, false), 1.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::Wpc, 6, false), 2.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::Wpc, 7, false), 1.f));
   CHECK_MSG(Near(Overlay::Luminance(Platform::Wpc, 0x78, false), 0.f), "bits WPC does not use are ignored");

   // Data East family: v & 3 of 3, bit 1 the long scan.
   CHECK(Near(Overlay::Luminance(Platform::DataEast, 1, false), 1.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::DataEast, 2, false), 2.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::DataEast, 3, false), 1.f));
   CHECK(Near(Overlay::Luminance(Platform::DataEast, 0x7c, false), 0.f));

   // Gottlieb System 3: popcount(v & 0x3F) of 6.
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 0x3f, false), 1.f));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 0x15, false), 0.5f));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 0x01, false), 1.f / 6));
   CHECK(Near(Overlay::Luminance(Platform::Gts3, 0x40, false), 0.f));

   // Capcom and Alvin G: popcount(v & 0xF) of 4.
   for (const Platform p : { Platform::Capcom, Platform::AlvinG })
   {
      CHECK(Near(Overlay::Luminance(p, 0x0f, true), 1.f));
      CHECK(Near(Overlay::Luminance(p, 0x05, true), 0.5f));
      CHECK(Near(Overlay::Luminance(p, 0x08, true), 0.25f));
      CHECK(Near(Overlay::Luminance(p, 0x70, true), 0.f));
   }

   // SAM: PinMAME's own LUT for the framebuffer value, 8 to 11 darker than 7.
   CHECK(Near(Overlay::Luminance(Platform::Sam, 0, true), 0.f));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 7, true), 7.f / 12));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 8, true), 5.f / 12));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 11, true), 8.f / 12));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 12, true), 9.f / 12));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 15, true), 1.f));
   CHECK(Near(Overlay::Luminance(Platform::Sam, 0x1f, true), 1.f));

   // Unknown: WPC's rule on a 4-shade display, the low nibble on a 16-shade one.
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 3, false), 2.f / 3));
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 5, true), 5.f / 15));
   CHECK(Near(Overlay::Luminance(Platform::Unknown, 15, true), 1.f));
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
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 1000, t0);
   CHECK_MSG(!s.Visible(t0), "nothing to show yet");

   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   CHECK_MSG(s.Visible(t0 + milliseconds(500)), "an upload inside the window shows at once");
   CHECK_MSG(!s.Visible(t0 + milliseconds(1000)), "hidden at the deadline");
}

void TestUploadAfterExpiryStaysHidden()
{
   Overlay::State s;
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 100, t0);
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   CHECK(!s.Visible(t0 + milliseconds(200)));
}

void TestTimerRestartsAndUntilHidden()
{
   Overlay::State s;
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 100, t0);
   s.Control(true, 100, t0 + milliseconds(90));
   CHECK_MSG(s.Visible(t0 + milliseconds(150)), "control restarts the timer");
   CHECK(!s.Visible(t0 + milliseconds(190)));

   s.Control(true, 0, t0);
   CHECK_MSG(s.Visible(t0 + std::chrono::hours(1)), "0 shows until hidden");
   s.Control(false, 500, t0);
   CHECK_MSG(!s.Visible(t0), "hide ignores its duration");
}

void TestUploadWhileShownReplaces()
{
   Overlay::State s;
   const std::vector<uint8_t> a = Payload(10, 10, 0x01), b = Payload(12, 10, 0x02);
   s.Store(a.data(), a.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 0, t0);
   s.Store(b.data(), b.size());
   CHECK(s.Visible(t0));
   CHECK(s.Width() == 12 && s.Pixels() == std::vector<uint8_t>(120, 0x02));
}

void TestClearDropsBitmapAndTimer()
{
   Overlay::State s;
   const std::vector<uint8_t> p = Payload(10, 10, 0x07);
   s.Store(p.data(), p.size());
   const Clock::time_point t0 = Clock::now();
   s.Control(true, 0, t0);
   s.Clear();
   CHECK(!s.Visible(t0));
   CHECK(s.Pixels().empty() && s.Width() == 0 && s.Height() == 0);
   // A new session starts with no overlay armed: an upload alone shows nothing.
   s.Store(p.data(), p.size());
   CHECK(!s.Visible(t0));
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
