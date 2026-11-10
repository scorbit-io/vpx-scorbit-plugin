// license:GPLv3+

// The show / hide sweep against the probe firmware's rules, on a fake clock. Expected positions
// are worked by hand from those rules (a band `width` columns wide starts `width` columns off the
// left and steps one column a pace), not taken from the header under test.

#include "OverlayState.h"

#include "Check.h"

#include <array>
#include <chrono>
#include <cmath>
#include <set>
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
   TestSparkleStatistics();
   return ScorbitTest::Summary("overlay_transition_test");
}
