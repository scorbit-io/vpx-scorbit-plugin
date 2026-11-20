// license:GPLv3+

// The overlaid identify frame (wire protocol 1.3): the composer against a dot-by-dot reference
// built on ComposeDot, the rule the render path draws by; its shade mapping; when overlay_seq
// advances. Which display the render path places on is checked by socket_worker.

#include "Check.h"
#include "OverlaidFrame.h"

#include <chrono>
#include <cmath>
#include <vector>

using namespace Scorbit;
using Overlay::Platform;

namespace
{

using Clock = Overlay::OverlaidComposer::Clock;
using std::chrono::milliseconds;

std::vector<uint8_t> Upload(uint8_t w, uint8_t h, uint8_t fill)
{
   std::vector<uint8_t> p { w, h };
   p.resize(2 + static_cast<size_t>(w) * h, fill);
   return p;
}

struct Plain
{
   uint32_t frameId = 1;
   uint32_t generation = 1;
   uint16_t width = 128;
   uint16_t height = 32;
   uint8_t shades = 4;
   std::vector<uint8_t> pixels;

   Plain(uint16_t w, uint16_t h, uint8_t s)
      : width(w), height(h), shades(s), pixels(static_cast<size_t>(w) * h)
   {
      for (size_t i = 0; i < pixels.size(); i++)
         pixels[i] = static_cast<uint8_t>(i % s);
   }

   Overlay::PlainFrame View() const { return { true, frameId, generation, width, height, shades, pixels.data() }; }
};

// The wire definition, in doubles, as written.
uint8_t SpecShade(int P, int S, uint8_t v)
{
   const long p = std::lround((v & 0x0F) * (P - 1) / 15.0);
   return static_cast<uint8_t>(std::lround(p * (S - 1) / static_cast<double>(P - 1)));
}

// Every dot through ComposeDot, the rule DmdOverlay::Composite draws by, with no row skipping.
std::vector<uint8_t> Reference(const Overlay::Cache& cache, Platform platform, const Plain& plain, Clock::time_point now)
{
   static Overlay::Record rec, scratch;
   uint64_t seq = 0;
   cache.ReadIfChanged(seq, rec, scratch);
   const Overlay::Sweep sweep = rec.SweepAt(now, plain.width);
   std::vector<uint8_t> out = plain.pixels;
   if (!sweep.Visible())
      return out;
   const Overlay::Placement at = Overlay::Place(plain.width, plain.height, rec.width, rec.height);
   const int P = Overlay::Shades(platform, plain.shades == 16);
   for (uint32_t y = 0; y < plain.height; y++)
      for (uint32_t x = 0; x < plain.width; x++)
         if (const int v = Overlay::ComposeDot(rec, at, sweep, x, y); v >= 0)
            out[static_cast<size_t>(y) * plain.width + x] = SpecShade(P, plain.shades, static_cast<uint8_t>(v));
   return out;
}

void TestShadeMatchesTheWireDefinition()
{
   for (const int P : { 4, 5, 7, 16 })
      for (const int S : { 4, 16 })
         for (int v = 0; v < 128; v++)
            CHECK_MSG(Overlay::IdentifyShade(P, S, static_cast<uint8_t>(v)) == SpecShade(P, S, static_cast<uint8_t>(v)),
               "P " + std::to_string(P) + " S " + std::to_string(S) + " v " + std::to_string(v));
   // P = S is one rounding; GTS3's tie at p = 1 rounds up.
   CHECK(Overlay::IdentifyShade(16, 16, 0x09) == 9);
   CHECK(Overlay::IdentifyShade(4, 4, 0x0F) == 3);
   CHECK(Overlay::IdentifyShade(7, 16, 0x02) == 3);
   CHECK_MSG(Overlay::IdentifyShade(4, 4, 0x8F) == 3, "bits 4 to 7 are ignored");
}

void TestNothingShowingIsPlain()
{
   Overlay::Cache cache;
   Overlay::OverlaidComposer composer;
   const Plain plain(128, 32, 4);
   const auto now = Clock::now();
   Overlay::OverlaidFrame f = composer.Compose(cache, Platform::Wpc, true, plain.View(), now);
   CHECK(!f.showing && f.pixels == nullptr);

   // A bitmap with no show is still not on the display.
   const std::vector<uint8_t> p = Upload(16, 10, 0x0F);
   CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
   f = composer.Compose(cache, Platform::Wpc, true, plain.View(), now);
   CHECK(!f.showing);

   // No conforming source: nothing to compose on.
   cache.SetTransition({ .on = false });
   cache.Control(true, 0);
   Overlay::PlainFrame none;
   f = composer.Compose(cache, Platform::Wpc, true, none, Clock::now());
   CHECK(!f.showing);

   // No surface: VPX draws no overlay, so neither does the feed.
   f = composer.Compose(cache, Platform::Wpc, false, plain.View(), Clock::now());
   CHECK(!f.showing);
   f = composer.Compose(cache, Platform::Wpc, true, plain.View(), Clock::now());
   CHECK(f.showing);
}

void TestMatchesComposeDot()
{
   struct Case
   {
      uint16_t w, h;
      uint8_t S;
      Platform platform;
      uint8_t ow, oh;
   };
   const Case cases[] = {
      { 128, 32, 4, Platform::Wpc, 64, 16 },
      { 128, 32, 16, Platform::Sam, 128, 32 },
      { 128, 32, 16, Platform::Gts3, 100, 20 },      // P 7 onto S 16
      { 128, 32, 4, Platform::Capcom, 130, 40 },     // P 5 onto S 4, cropped
      { 128, 16, 4, Platform::DataEast, 48, 16 },
      { 192, 64, 4, Platform::DataEast, 192, 64 },
      { 128, 32, 4, Platform::Unknown, 32, 32 },
   };
   for (const Case& c : cases)
   {
      Overlay::Cache cache;
      cache.SetTransition({ .on = false });
      std::vector<uint8_t> p = Upload(c.ow, c.oh, 0);
      for (size_t i = 2; i < p.size(); i++)
         p[i] = static_cast<uint8_t>(i % 7 == 0 ? 0x80 | 0x0F : i % 16);   // every brightness, some transparent
      CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
      cache.Control(true, 0);
      const Plain plain(c.w, c.h, c.S);
      const auto now = Clock::now();
      Overlay::OverlaidComposer composer;
      const Overlay::OverlaidFrame f = composer.Compose(cache, c.platform, true, plain.View(), now);
      CHECK(f.showing && f.pixels != nullptr);
      if (f.pixels != nullptr)
         CHECK_MSG(*f.pixels == Reference(cache, c.platform, plain, now),
            std::to_string(c.w) + "x" + std::to_string(c.h) + " overlay " + std::to_string(c.ow) + "x" + std::to_string(c.oh));
   }
}

void TestSweepsMatchComposeDot()
{
   using Overlay::Effect;
   for (const Effect effect : { Effect::Sparkle, Effect::Bar, Effect::Meteor, Effect::Glow, Effect::Line, Effect::Ticker })
   {
      Overlay::Cache cache;
      cache.SetTransition({ .on = true, .effect = effect, .paceMs = 10, .width = 8 });
      std::vector<uint8_t> p = Upload(64, 16, 0x0C);
      p[2 + 5] = 0x80;   // one transparent dot
      CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
      cache.Control(true, 2000);
      const Plain plain(128, 32, 16);
      Overlay::OverlaidComposer composer;
      const auto start = Clock::now();
      uint32_t lastSeq = 0;
      int advances = 0;
      // Through the show wipe, the hold, the hide wipe and past it.
      for (int ms = 0; ms <= 4500; ms += 37)
      {
         const auto now = start + milliseconds(ms);
         const Overlay::OverlaidFrame f = composer.Compose(cache, Platform::Sam, true, plain.View(), now);
         const std::vector<uint8_t> want = Reference(cache, Platform::Sam, plain, now);
         if (f.showing)
            CHECK_MSG(f.pixels != nullptr && *f.pixels == want, "effect " + std::to_string(static_cast<int>(effect)) + " at " + std::to_string(ms));
         else
            CHECK_MSG(want == plain.pixels, "hidden means plain, at " + std::to_string(ms));
         if (ms != 0 && f.seq != lastSeq)
            advances++;
         lastSeq = f.seq;
      }
      CHECK_MSG(advances > 20, "sweep steps advance overlay_seq: " + std::to_string(advances));
      CHECK(!composer.Compose(cache, Platform::Sam, true, plain.View(), start + milliseconds(10000)).showing);
   }
}

void TestSeqAdvancesOnItsInputsOnly()
{
   Overlay::Cache cache;
   cache.SetTransition({ .on = false });
   Overlay::OverlaidComposer composer;
   Plain plain(128, 32, 4);
   const auto now = Clock::now();

   // Hidden: the plain frame is not an input.
   const uint32_t hidden = composer.Compose(cache, Platform::Wpc, true, plain.View(), now).seq;
   plain.frameId = 2;
   CHECK_MSG(composer.Compose(cache, Platform::Wpc, true, plain.View(), now).seq == hidden, "a plain change while hidden");
   CHECK(composer.Compose(cache, Platform::Wpc, true, plain.View(), now + milliseconds(500)).seq == hidden);

   // The record.
   const std::vector<uint8_t> p = Upload(32, 10, 0x0F);
   CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
   const uint32_t uploaded = composer.Compose(cache, Platform::Wpc, true, plain.View(), now).seq;
   CHECK(uploaded != hidden);

   // overlay_state.
   cache.Control(true, 0);
   const auto shown = Clock::now();
   Overlay::OverlaidFrame f = composer.Compose(cache, Platform::Wpc, true, plain.View(), shown);
   CHECK(f.showing && f.seq != uploaded);
   const uint32_t showing = f.seq;

   // Nothing changed: no advance, at any time while it holds.
   CHECK(composer.Compose(cache, Platform::Wpc, true, plain.View(), shown).seq == showing);
   CHECK(composer.Compose(cache, Platform::Wpc, true, plain.View(), shown + milliseconds(800)).seq == showing);

   // The plain frame, while showing: id, then generation.
   plain.frameId = 3;
   const uint32_t newFrame = composer.Compose(cache, Platform::Wpc, true, plain.View(), shown).seq;
   CHECK(newFrame != showing);
   plain.generation = 2;
   const uint32_t newGeneration = composer.Compose(cache, Platform::Wpc, true, plain.View(), shown).seq;
   CHECK(newGeneration != newFrame);

   // P, once the platform is known: GTS3 shows 7 levels where the depth alone says 4.
   const uint32_t gts3 = composer.Compose(cache, Platform::Gts3, true, plain.View(), shown).seq;
   CHECK(gts3 != newGeneration);
   CHECK_MSG(composer.Compose(cache, Platform::Wpc, true, plain.View(), shown).seq != gts3, "back to WPC's 4");
   const uint32_t wpc = composer.Compose(cache, Platform::Wpc, true, plain.View(), shown).seq;
   CHECK_MSG(composer.Compose(cache, Platform::DataEast, true, plain.View(), shown).seq == wpc, "same depth, same frame");

   // Hidden again.
   cache.Control(false, 0);
   f = composer.Compose(cache, Platform::Wpc, true, plain.View(), Clock::now());
   CHECK(!f.showing && f.seq != wpc);
}

void TestTheTimerExpires()
{
   Overlay::Cache cache;
   cache.SetTransition({ .on = false });
   const std::vector<uint8_t> p = Upload(32, 10, 0x0F);
   CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
   cache.Control(true, 300);
   const auto start = Clock::now();
   Overlay::OverlaidComposer composer;
   const Plain plain(128, 32, 4);
   const Overlay::OverlaidFrame on = composer.Compose(cache, Platform::Wpc, true, plain.View(), start);
   CHECK(on.showing);
   const Overlay::OverlaidFrame off = composer.Compose(cache, Platform::Wpc, true, plain.View(), start + milliseconds(1000));
   CHECK_MSG(!off.showing && off.seq != on.seq, "expiry with no writer and no new game frame");
}

}

int main()
{
   TestShadeMatchesTheWireDefinition();
   TestNothingShowingIsPlain();
   TestMatchesComposeDot();
   TestSweepsMatchComposeDot();
   TestSeqAdvancesOnItsInputsOnly();
   TestTheTimerExpires();
   return ScorbitTest::Summary("overlaid_frame_test");
}
