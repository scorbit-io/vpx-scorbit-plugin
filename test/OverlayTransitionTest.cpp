// license:GPLv3+

// The show / hide sweep against the probe firmware's rules, on a fake clock. Expected positions
// are worked by hand from those rules (a band `width` columns wide starts `width` columns off the
// left and steps one column a pace), not taken from the header under test.

#include "OverlayCache.h"
#include "OverlayState.h"

#include "Check.h"

#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Scorbit;
using Overlay::Reveal;
using Clock = Overlay::State::Clock;
using std::chrono::milliseconds;

namespace
{

constexpr uint32_t W = 128;
const Clock::time_point T0 = Clock::time_point { } + std::chrono::hours(1);

Clock::time_point At(int ms)
{
   return T0 + milliseconds(ms);
}

Overlay::State Shown()
{
   Overlay::State s;
   std::vector<uint8_t> p { 128, 32 };
   p.resize(2 + 128 * 32, 0x0F);
   s.Store(p.data(), p.size());
   return s;
}

Overlay::Sweep SweepOf(const Overlay::State& s, int ms, uint32_t width = W)
{
   return s.SweepAt(At(ms), width);
}

void TestShowSweepsAcross()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));

   Overlay::Sweep w = SweepOf(s, 0);
   CHECK_MSG(w.reveal == Reveal::Show && w.frontier == -8 && w.width == 8, "starts a band's width off the left");
   CHECK_MSG(!w.ShowsOverlay(0) && w.BandColumn(0) == -1, "nothing on the display yet");

   CHECK(SweepOf(s, 9).frontier == -8);
   CHECK_MSG(SweepOf(s, 10).frontier == -7, "one column every 10 ms");
   w = SweepOf(s, 85);
   CHECK_MSG(w.frontier == 0 && w.BandColumn(0) == 0 && w.BandColumn(7) == 7 && w.BandColumn(8) == -1, "the band's head leads, on the left");

   // A read long after the last one lands on the right step: the firmware's catch-up.
   w = SweepOf(s, 1000);
   CHECK(w.reveal == Reveal::Show && w.frontier == 92);
   CHECK_MSG(w.ShowsOverlay(91) && !w.ShowsOverlay(92) && !w.ShowsOverlay(100), "overlay left of the band, game right of it");
   CHECK(w.BandColumn(92) == 0 && w.BandColumn(99) == 7 && w.BandColumn(100) == -1);

   CHECK_MSG(SweepOf(s, 1359).reveal == Reveal::Show && SweepOf(s, 1359).frontier == 127, "still on the last column");
   CHECK_MSG(SweepOf(s, 1360).reveal == Reveal::Full, "(128 + 8) x 10 ms = 1.36 s");
   CHECK_MSG(SweepOf(s, 1999, 192).reveal == Reveal::Show && SweepOf(s, 2000, 192).reveal == Reveal::Full, "192 wide: 2.0 s");
}

void TestHideMirrorsAndEnds()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.Control(false, 0, At(2000));

   Overlay::Sweep w = SweepOf(s, 2000);
   CHECK(w.reveal == Reveal::Hide && w.frontier == -8);
   w = SweepOf(s, 2500);
   CHECK(w.reveal == Reveal::Hide && w.frontier == 42);
   CHECK_MSG(w.BandColumn(42) == 7 && w.BandColumn(49) == 0, "mirrored: the head faces the overlay, on the right");
   CHECK_MSG(!w.ShowsOverlay(41) && w.ShowsOverlay(50) && w.ShowsOverlay(127), "game left of the band, overlay right of it");
   CHECK(SweepOf(s, 3359).reveal == Reveal::Hide);
   CHECK_MSG(SweepOf(s, 3360).reveal == Reveal::Off, "gone after another 1.36 s");
}

void TestHideDuringShowKeepsWhatWasShown()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.Control(false, 0, At(500));   // the show had reached column 42

   Overlay::Sweep w = SweepOf(s, 700);   // the hide has stepped 20 times: frontier 12
   CHECK(w.reveal == Reveal::Hide && w.frontier == 12 && w.limit == 42);
   CHECK_MSG(w.ShowsOverlay(20) && w.ShowsOverlay(41), "the columns already shown stay until the band reaches them");
   CHECK_MSG(!w.ShowsOverlay(42) && !w.ShowsOverlay(100), "columns the show never reached stay game");
   CHECK(SweepOf(s, 999).reveal == Reveal::Hide);
   CHECK_MSG(SweepOf(s, 1000).reveal == Reveal::Off, "ends when the band reaches what was shown: 50 steps");

   Overlay::State early = Shown();
   early.Control(true, 0, At(0));
   early.Control(false, 0, At(50));   // frontier -3: nothing on the display yet
   CHECK_MSG(SweepOf(early, 50).reveal == Reveal::Off, "nothing shown, so nothing to sweep out");
}

void TestShowDuringHideRestartsFromTheLeft()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.Control(false, 0, At(2000));
   s.Control(true, 0, At(2300));
   Overlay::Sweep w = SweepOf(s, 2300);
   CHECK(w.reveal == Reveal::Show && w.frontier == -8);
   CHECK_MSG(SweepOf(s, 3660).reveal == Reveal::Full, "a full show from there");
}

void TestOnlyAnEdgeStartsASweep()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.Control(true, 0, At(500));
   CHECK_MSG(SweepOf(s, 500).frontier == 42, "an enable while showing does not restart the sweep");
   CHECK(SweepOf(s, 1360).reveal == Reveal::Full);
   s.Control(true, 0, At(2000));
   CHECK_MSG(SweepOf(s, 2000).reveal == Reveal::Full, "nor while shown");

   // An upload while shown swaps at once.
   std::vector<uint8_t> p { 128, 32 };
   p.resize(2 + 128 * 32, 0x05);
   s.Store(p.data(), p.size());
   CHECK(SweepOf(s, 2000).reveal == Reveal::Full && s.Pixels()[0] == 0x05);

   Overlay::State hidden = Shown();
   hidden.Control(false, 0, At(0));
   CHECK_MSG(SweepOf(hidden, 0).reveal == Reveal::Off, "a hide while hidden does nothing");
}

void TestHoldThenHide()
{
   Overlay::State s = Shown();
   s.Control(true, 5000, At(0));
   CHECK(SweepOf(s, 4999).reveal == Reveal::Full);
   const Overlay::Sweep w = SweepOf(s, 5000);
   CHECK_MSG(w.reveal == Reveal::Hide && w.frontier == -8, "the hold counts from the control; the hide sweep starts at its end");
   CHECK(SweepOf(s, 6359).Visible());
   CHECK_MSG(!SweepOf(s, 6360).Visible(), "and runs to its end with no writer");

   // A timer that runs out during the show interrupts it like a hide.
   Overlay::State quick = Shown();
   quick.Control(true, 500, At(0));
   CHECK(SweepOf(quick, 700).reveal == Reveal::Hide && SweepOf(quick, 700).limit == 42);

   // A control after expiry sees the hide the timer began.
   s.Control(true, 0, At(5300));
   CHECK_MSG(SweepOf(s, 5300).reveal == Reveal::Show && SweepOf(s, 5300).frontier == -8, "a show during the timer's hide restarts it");
}

void TestForceAndDisable()
{
   Overlay::State on = Shown();
   on.Control(true, 0, At(0), false, true);
   CHECK_MSG(SweepOf(on, 0).reveal == Reveal::Full, "setting on, disable bit: shown at once");
   on.Control(false, 0, At(100), false, true);
   CHECK_MSG(SweepOf(on, 100).reveal == Reveal::Off, "and hidden at once");
   on.Control(true, 0, At(200), true, false);
   CHECK_MSG(SweepOf(on, 200).reveal == Reveal::Show, "force with the setting on: a sweep");

   Overlay::State off = Shown();
   off.SetTransition({ .on = false });
   off.Control(true, 0, At(0));
   CHECK_MSG(SweepOf(off, 0).reveal == Reveal::Full, "setting off: no sweep");
   off.Control(false, 0, At(100));
   CHECK(SweepOf(off, 100).reveal == Reveal::Off);
   off.Control(true, 0, At(200), true, false);
   CHECK_MSG(SweepOf(off, 200).reveal == Reveal::Show, "setting off, force bit: a sweep");
   off.Control(false, 0, At(2000), true, true);
   CHECK_MSG(SweepOf(off, 2000).reveal == Reveal::Hide, "setting off, both bits: the force bit decides");

   // A control that does not transition settles a sweep in progress, as the firmware's next frame does.
   Overlay::State mid = Shown();
   mid.Control(true, 0, At(0));
   mid.Control(true, 0, At(300), false, true);
   CHECK(SweepOf(mid, 300).reveal == Reveal::Full);

   Overlay::State narrow = Shown();
   narrow.SetTransition({ .width = 0 });
   narrow.Control(true, 0, At(0), true, false);
   CHECK_MSG(SweepOf(narrow, 0).reveal == Reveal::Full, "width 0 never sweeps, forced or not");
}

void TestWipeKeepsItsShape()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.SetTransition({ .paceMs = 20, .width = 4 });
   const Overlay::Sweep w = SweepOf(s, 1000);
   CHECK_MSG(w.frontier == 92 && w.width == 8, "a running wipe keeps the pace and width it started with");
   s.Control(false, 0, At(2000));
   CHECK_MSG(SweepOf(s, 2000).frontier == -4 && SweepOf(s, 2100).frontier == 1, "the next one takes the new settings");
}

void TestClearDropsASweep()
{
   Overlay::State s = Shown();
   s.Control(true, 0, At(0));
   s.Clear();
   CHECK(!SweepOf(s, 100).Visible());
   std::vector<uint8_t> p { 128, 32 };
   p.resize(2 + 128 * 32, 0x0F);
   s.Store(p.data(), p.size());
   CHECK_MSG(!SweepOf(s, 200).Visible(), "a new session starts hidden");
}

void TestBandWithoutABitmap()
{
   Overlay::State s;
   s.Control(true, 0, At(0));
   CHECK_MSG(SweepOf(s, 500).reveal == Reveal::Show, "the band crosses the game before any upload, as the firmware's does");
   CHECK_MSG(!SweepOf(s, 1360).Visible(), "and nothing is left once it has passed");
   std::vector<uint8_t> p { 128, 32 };
   p.resize(2 + 128 * 32, 0x0F);
   s.Store(p.data(), p.size());
   CHECK_MSG(SweepOf(s, 1400).reveal == Reveal::Full, "an upload then shows at once");
}

// One base dot at a time, as Composite draws every output dot that maps to it.
void TestComposeDot()
{
   // A 128x32 overlay: opaque value 0x0A in columns 0 to 63, transparent beyond.
   auto rec = std::make_unique<Overlay::Record>();
   rec->width = 128;
   rec->height = 32;
   for (uint32_t y = 0; y < 32; y++)
      for (uint32_t x = 0; x < 128; x++)
         rec->pixels[y * 128 + x] = x < 64 ? 0x0A : 0x8A;
   const Overlay::Placement at = Overlay::Place(128, 32, 128, 32);

   Overlay::Sweep full;
   full.reveal = Reveal::Full;
   CHECK(Overlay::ComposeDot(*rec, at, full, 10, 5) == 0x0A);
   CHECK_MSG(Overlay::ComposeDot(*rec, at, full, 100, 5) == -1, "a transparent pixel shows the game");

   Overlay::Sweep show;
   show.reveal = Reveal::Show;
   show.frontier = 50;
   show.width = 8;
   show.limit = 128;
   show.seed = 3;
   CHECK_MSG(Overlay::ComposeDot(*rec, at, show, 49, 5) == 0x0A, "revealed left of the band");
   CHECK_MSG(Overlay::ComposeDot(*rec, at, show, 60, 5) == -1, "the game right of the band, under an opaque pixel too");
   int lit = 0, game = 0;
   for (uint32_t y = 0; y < 32; y++)
      for (uint32_t x = 50; x < 58; x++)
      {
         const int v = Overlay::ComposeDot(*rec, at, show, x, y);
         CHECK_MSG(v == -1 || v == 9 || v == 12 || v == 15, "a band dot is lit or shows the game, never the overlay");
         (v == -1 ? game : lit)++;
      }
   CHECK(lit > 0 && game > 0);

   Overlay::Sweep hide;
   hide.reveal = Reveal::Hide;
   hide.frontier = 20;
   hide.width = 8;
   hide.limit = 42;   // a hide that interrupted a show at column 42
   hide.seed = 3;
   CHECK_MSG(Overlay::ComposeDot(*rec, at, hide, 19, 5) == -1, "masked left of the band");
   CHECK_MSG(Overlay::ComposeDot(*rec, at, hide, 30, 5) == 0x0A, "still shown right of it");
   CHECK_MSG(Overlay::ComposeDot(*rec, at, hide, 50, 5) == -1, "never shown past the limit");

   // A smaller overlay is centred: 64x16 on 128x32 lands at (32, 8).
   rec->width = 64;
   rec->height = 16;
   for (uint32_t i = 0; i < 64 * 16; i++)
      rec->pixels[i] = 0x05;
   const Overlay::Placement centred = Overlay::Place(128, 32, 64, 16);
   CHECK(Overlay::ComposeDot(*rec, centred, show, 32, 8) == 0x05);
   CHECK(Overlay::ComposeDot(*rec, centred, show, 31, 8) == -1 && Overlay::ComposeDot(*rec, centred, show, 32, 7) == -1);
   CHECK_MSG(Overlay::ComposeDot(*rec, centred, full, 95, 23) == 0x05 && Overlay::ComposeDot(*rec, centred, full, 96, 23) == -1, "its far edge");
}

// The record the render threads read carries the sweep, timer included, with no writer after the control.
void TestCacheCarriesTheSweep()
{
   Overlay::Cache cache;
   std::vector<uint8_t> p { 128, 32 };
   p.resize(2 + 128 * 32, 0x0F);
   cache.Store(p.data(), p.size());
   const Clock::time_point before = Clock::now();
   cache.Control(true, 2000);
   const Clock::time_point after = Clock::now();
   auto record = std::make_unique<Overlay::Record>();
   auto scratch = std::make_unique<Overlay::Record>();
   uint64_t seq = 0;
   CHECK(cache.ReadIfChanged(seq, *record, *scratch));
   CHECK_MSG(record->SweepAt(after, 128).reveal == Reveal::Show, "a show sweeps in");
   CHECK(record->SweepAt(after + milliseconds(1500), 128).reveal == Reveal::Full);
   CHECK_MSG(record->SweepAt(after + milliseconds(2050), 128).reveal == Reveal::Hide, "the hold ends in a hide sweep");
   CHECK_MSG(!record->Visible(after + milliseconds(2000 + 1360 + 10), 128), "which ends");
   CHECK(record->Visible(before + milliseconds(2000 + 1360 - 10), 128));

   cache.Control(false, 0, false, true);
   CHECK(cache.ReadIfChanged(seq, *record, *scratch));
   CHECK_MSG(!record->Visible(Clock::now(), 128), "a hide with the disable bit is immediate");
}

Overlay::Sweep Band(Overlay::Effect effect, int32_t width)
{
   Overlay::Sweep w;
   w.reveal = Reveal::Show;
   w.effect = effect;
   w.width = width;
   w.seed = 5;
   return w;
}

void TestBarLineAndGlow()
{
   Overlay::Sweep bar = Band(Overlay::Effect::Bar, 8);
   bool solid = true;
   for (int32_t y = 0; y < 32; y++)
      for (int32_t x = 0; x < 8; x++)
         solid = solid && Overlay::BandDot(bar, x, y) == 15;
   CHECK_MSG(solid, "a bar is every dot at 15");

   // A line is a bar one column wide, whatever the width setting: (128 + 1) x 10 ms.
   Overlay::State s = Shown();
   s.SetTransition({ .effect = Overlay::Effect::Line, .width = 8 });
   s.Control(true, 0, At(0));
   Overlay::Sweep w = SweepOf(s, 500);
   CHECK(w.frontier == 49 && w.width == 1 && w.BandColumn(49) == 0 && w.BandColumn(50) == -1);
   CHECK(Overlay::BandDot(w, 0, 3) == 15);
   CHECK(SweepOf(s, 1289).reveal == Reveal::Show && SweepOf(s, 1290).reveal == Reveal::Full);

   // Glow: 15, then the last three columns 12, 9, 5.
   const Overlay::Sweep glow = Band(Overlay::Effect::Glow, 8);
   const int expected[8] = { 15, 15, 15, 15, 15, 12, 9, 5 };
   for (int32_t x = 0; x < 8; x++)
      CHECK_MSG(Overlay::BandDot(glow, x, 0) == expected[x], "glow column " + std::to_string(x));
   // Narrow bands keep the head at 15 and start the tail at column 1.
   CHECK(Overlay::BandDot(Band(Overlay::Effect::Glow, 2), 0, 0) == 15 && Overlay::BandDot(Band(Overlay::Effect::Glow, 2), 1, 0) == 12);
   CHECK(Overlay::BandDot(Band(Overlay::Effect::Glow, 4), 1, 0) == 12 && Overlay::BandDot(Band(Overlay::Effect::Glow, 4), 3, 0) == 5);
   CHECK(Overlay::BandDot(Band(Overlay::Effect::Glow, 1), 0, 0) == 15);

   // Mirrored on hide: the head, 15, is the band's rightmost column.
   Overlay::Sweep hide = glow;
   hide.reveal = Reveal::Hide;
   hide.frontier = 40;
   CHECK(Overlay::BandDot(hide, hide.BandColumn(47), 0) == 15 && Overlay::BandDot(hide, hide.BandColumn(40), 0) == 5);
}

// Meteor: per row, a trail from the head of 1 to width columns, fading 15 - x * 12 / (width - 1), at least 3.
void TestMeteor()
{
   Overlay::Sweep w = Band(Overlay::Effect::Meteor, 8);
   const int shade[8] = { 15, 14, 12, 10, 9, 7, 5, 3 };   // 15 - floor(12x / 7)
   int rows = 0, full = 0, bad = 0;
   for (int32_t f = 0; f < 2000; f++)
   {
      w.frontier = f;
      for (int32_t y = 0; y < 32; y++)
      {
         int trail = 0;
         while (trail < 8 && Overlay::BandDot(w, trail, y) != 0)
            trail++;
         for (int32_t x = 0; x < 8; x++)
         {
            const uint8_t v = Overlay::BandDot(w, x, y);
            if ((x < trail && v != shade[x]) || (x >= trail && v != 0))
               bad++;
         }
         bad += trail == 0 ? 1 : 0;
         rows++;
         full += trail == 8 ? 1 : 0;
      }
   }
   CHECK_MSG(bad == 0, "every row a contiguous trail from the head, at the firmware's shades");
   // Full width when (r & 0x30) == 0, a quarter of rows, or when 1 + r % 8 is 8 in the rest: 1/4 + 3/4 x 1/8.
   CHECK_MSG(std::fabs(static_cast<double>(full) / rows - 0.34375) < 0.01, "full-width trails at the firmware's rate");
}

// Ticker: no band. Show slides the overlay in from a display's width off the left, one column a
// pace; hide slides it out to the right, a hide during the slide carrying on from where it is.
void TestTicker()
{
   Overlay::State s = Shown();
   s.SetTransition({ .effect = Overlay::Effect::Ticker, .width = 0 });
   s.Control(true, 0, At(0));
   Overlay::Sweep w = SweepOf(s, 0);
   CHECK_MSG(w.reveal == Reveal::Show && w.frontier == -128, "starts fully off the left; width 0 does not stop it");
   w = SweepOf(s, 500);
   CHECK(w.frontier == -78 && w.SourceColumn(100) == 178 && w.SourceColumn(10) == 88 && w.BandColumn(10) == -1);
   CHECK(SweepOf(s, 1279).reveal == Reveal::Show && SweepOf(s, 1280).reveal == Reveal::Full);

   s.Control(false, 0, At(2000));
   w = SweepOf(s, 2300);
   CHECK_MSG(w.reveal == Reveal::Hide && w.frontier == 30 && w.SourceColumn(40) == 10, "hide slides out to the right");
   CHECK(SweepOf(s, 3279).reveal == Reveal::Hide && SweepOf(s, 3280).reveal == Reveal::Off);

   Overlay::State mid = Shown();
   mid.SetTransition({ .effect = Overlay::Effect::Ticker });
   mid.Control(true, 0, At(0));
   mid.Control(false, 0, At(500));   // slid in to -78
   CHECK_MSG(SweepOf(mid, 600).frontier == -68, "a hide during the slide carries on from its offset");
   CHECK_MSG(SweepOf(mid, 2559).reveal == Reveal::Hide && SweepOf(mid, 2560).reveal == Reveal::Off, "206 steps to clear the right edge");

   Overlay::State none = Shown();
   none.SetTransition({ .effect = Overlay::Effect::Ticker });
   none.Control(true, 0, At(0));
   none.Control(false, 0, At(5));
   CHECK_MSG(!SweepOf(none, 5).Visible(), "still fully off the left: nothing to slide out");

   // Each dot shows the overlay column its offset puts there; past the overlay's edges, the game.
   auto rec = std::make_unique<Overlay::Record>();
   rec->width = 128;
   rec->height = 32;
   for (uint32_t y = 0; y < 32; y++)
      for (uint32_t x = 0; x < 128; x++)
         rec->pixels[y * 128 + x] = static_cast<uint8_t>(x < 120 ? x % 16 : 0x80);
   const Overlay::Placement at = Overlay::Place(128, 32, 128, 32);
   Overlay::Sweep slide;
   slide.reveal = Reveal::Show;
   slide.effect = Overlay::Effect::Ticker;
   slide.frontier = -78;
   CHECK_MSG(Overlay::ComposeDot(*rec, at, slide, 10, 0) == (88 % 16), "column 10 shows overlay column 88");
   CHECK_MSG(Overlay::ComposeDot(*rec, at, slide, 45, 0) == -1, "overlay column 123 is transparent: the game");
   CHECK_MSG(Overlay::ComposeDot(*rec, at, slide, 60, 0) == -1, "past the overlay's right edge: the game");
   slide.reveal = Reveal::Hide;
   slide.frontier = 30;
   CHECK(Overlay::ComposeDot(*rec, at, slide, 29, 0) == -1 && Overlay::ComposeDot(*rec, at, slide, 30, 0) == 0
      && Overlay::ComposeDot(*rec, at, slide, 47, 0) == 1);
}

// The sparkle's statistics against the firmware's table: lit with chance density / 256, then
// brightness 9, 12 or 15 at 1 : 1 : 2.
void TestSparkleStatistics()
{
   const std::array<int, 16> density { 124, 121, 125, 98, 92, 80, 68, 60, 48, 41, 30, 32, 15, 12, 6, 4 };
   Overlay::Sweep w;
   w.reveal = Reveal::Show;
   w.width = 8;
   w.seed = 7;
   std::array<int, 8> lit { };
   int bright[16] { };
   std::set<int> values;
   const int steps = 4000, rows = 32;
   for (int f = 0; f < steps; f++)
   {
      w.frontier = f;
      for (int y = 0; y < rows; y++)
         for (int x = 0; x < 8; x++)
         {
            const uint8_t v = Overlay::BandDot(w, x, y);
            values.insert(v);
            if (v != 0)
            {
               lit[x]++;
               bright[v]++;
            }
         }
   }
   CHECK_MSG(values == std::set<int>({ 0, 9, 12, 15 }), "only 9, 12 and 15 light");
   const double samples = static_cast<double>(steps) * rows;
   for (int x = 0; x < 8; x++)
   {
      // Width 8 samples the table at x * 15 / 7.
      const double expected = density[static_cast<size_t>(x * 15 / 7)] / 256.0;
      CHECK_MSG(std::fabs(lit[x] / samples - expected) < 0.01, "column " + std::to_string(x) + " lit at its density");
   }
   const double total = bright[9] + bright[12] + bright[15];
   CHECK(std::fabs(bright[9] / total - 0.25) < 0.01 && std::fabs(bright[12] / total - 0.25) < 0.01);

   // Drawn afresh every step, and the same for every reader of a step.
   int changed = 0;
   for (int y = 0; y < rows; y++)
   {
      w.frontier = 10;
      const uint8_t a = Overlay::BandDot(w, 0, y);
      CHECK(a == Overlay::BandDot(w, 0, y));
      w.frontier = 11;
      changed += a != Overlay::BandDot(w, 0, y) ? 1 : 0;
   }
   CHECK_MSG(changed > 0, "the next step draws a new band");

   w.width = 1;
   int head = 0;
   for (int f = 0; f < steps; f++)
   {
      w.frontier = f;
      for (int y = 0; y < rows; y++)
         head += Overlay::BandDot(w, 0, y) != 0 ? 1 : 0;
   }
   CHECK_MSG(std::fabs(head / samples - 124 / 256.0) < 0.01, "a band one column wide uses the head's density");
}

}

int main()
{
   TestShowSweepsAcross();
   TestHideMirrorsAndEnds();
   TestHideDuringShowKeepsWhatWasShown();
   TestShowDuringHideRestartsFromTheLeft();
   TestOnlyAnEdgeStartsASweep();
   TestHoldThenHide();
   TestForceAndDisable();
   TestWipeKeepsItsShape();
   TestClearDropsASweep();
   TestBandWithoutABitmap();
   TestComposeDot();
   TestCacheCarriesTheSweep();
   TestSparkleStatistics();
   TestBarLineAndGlow();
   TestMeteor();
   TestTicker();
   return ScorbitTest::Summary("overlay_transition_test");
}
