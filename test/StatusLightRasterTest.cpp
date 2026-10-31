// license:GPLv3+

// The software-drawn status light: its phases over time, and sample pixels of its frames. Expected
// values are worked out by hand from the geometry, the device cadences and premultiplied blending.

#include "StatusLightRaster.h"

#include "Check.h"

using namespace Scorbit;

namespace
{

LightView Idle()
{
   LightView v;
   v.visible = true;
   v.system = { true, Light::GREEN, 1200, 100 };
   v.session = { true, Light::BLUE, 1189, 123 };
   v.ring = LightRing::Rainbow;
   return v;
}

uint32_t At(const LightFrame& f, int x, int y)
{
   return f.pixels[static_cast<size_t>(y) * f.size + x];
}

LightPhase AllOn()
{
   return { true, true, 255, 0 };
}

void TestDotsBlinkAtTheDeviceCadence()
{
   const LightView v = Idle();
   CHECK(PhaseAt(v, 0, -1).systemLit && PhaseAt(v, 1.19, -1).systemLit);
   CHECK_MSG(!PhaseAt(v, 1.25, -1).systemLit, "the system dot is dark from 1200 to 1300 ms");
   CHECK(PhaseAt(v, 1.31, -1).systemLit);
   CHECK(PhaseAt(v, 1.18, -1).sessionLit);
   CHECK_MSG(!PhaseAt(v, 1.2, -1).sessionLit, "the session dot is dark from 1189 to 1312 ms");
   CHECK(PhaseAt(v, 1.32, -1).sessionLit);
   LightView off = v;
   off.system.lit = false;
   CHECK_MSG(!PhaseAt(off, 0, -1).systemLit, "an unlit dot never shows");
}

void TestRingPulseAndFlash()
{
   const LightView v = Idle();
   CHECK(PhaseAt(v, 0, -1).ring == 255);
   CHECK(PhaseAt(v, 0.75, -1).ring == 172);   // 1 - 0.65 * 0.5
   CHECK(PhaseAt(v, 1.5, -1).ring == 89);     // 0.35
   CHECK(PhaseAt(v, 3.0, -1).ring == 255);
   LightView solid = v;
   solid.ring = LightRing::Solid;
   CHECK(PhaseAt(solid, 1.5, -1).ring == 255);
   LightView dark = v;
   dark.ring = LightRing::Dark;
   CHECK(PhaseAt(dark, 0, -1).ring == 0);

   CHECK(PhaseAt(v, 0, -1).flash == 0);
   CHECK(PhaseAt(v, 0, 0.15).flash == 255);   // the peak, 30% in
   CHECK(PhaseAt(v, 0, 0.325).flash == 128);  // halfway down
   CHECK_MSG(PhaseAt(v, 0, 0.5).flash == 0, "the flash lasts half a second");
}

void TestFramePixels()
{
   LightFrame f;
   LightView v = Idle();
   Rasterize(v, AllOn(), 1, f);
   CHECK(f.size == 44 && f.pixels.size() == 44u * 44u);
   CHECK_MSG(At(f, 0, 0) == 0, "outside the disc is transparent");
   CHECK_MSG(At(f, 12, 22) == 0xFF00FF00u, "the system dot, opaque green");
   CHECK_MSG(At(f, 32, 22) == 0xFF0000FFu, "the session dot, opaque blue");
   CHECK_MSG(At(f, 22, 3) == 0xFFFF0000u, "the top ring segment is hue 0, red");
   CHECK_MSG(At(f, 22, 19) == 0x99000000u, "no glyph: the dark background");

   LightPhase dim = AllOn();
   dim.systemLit = false;
   dim.ring = 89;
   Rasterize(v, dim, 1, f);
   CHECK_MSG(At(f, 12, 22) == 0x99000000u, "a dark dot shows the background");
   CHECK_MSG(At(f, 22, 3) == 0xBD590000u, "red at 0.35 over black at 0.6");

   v.glyph = LightGlyph::Problem;
   Rasterize(v, AllOn(), 1, f);
   CHECK_MSG(At(f, 22, 19) == 0xFFFFFFFFu, "the bar of the !");
   CHECK_MSG(At(f, 22, 26) == 0xFFFFFFFFu, "the point of the !");
   v.glyph = LightGlyph::Unknown;
   Rasterize(v, AllOn(), 1, f);
   CHECK_MSG(At(f, 22, 26) == 0xFFFFFFFFu, "the point of the ?");

   LightView dark = Idle();
   dark.ring = LightRing::Dark;
   Rasterize(dark, AllOn(), 1, f);
   CHECK_MSG(At(f, 22, 3) == 0x99000000u, "a dark ring never draws, whatever the phase");

   LightView flash;
   flash.visible = true;
   Rasterize(flash, { false, false, 0, 255 }, 1, f);
   CHECK_MSG(At(f, 22, 3) == 0xFFFFFFFFu, "the claim flash is a white ring");
   CHECK_MSG(At(f, 12, 22) == 0x99000000u, "and nothing else");

   LightView solid = Idle();
   solid.ring = LightRing::Solid;
   solid.ringRgb = Light::BLUE;
   Rasterize(solid, AllOn(), 1, f);
   CHECK(At(f, 22, 3) == 0xFF0000FFu);

   Rasterize(Idle(), AllOn(), 2, f);
   CHECK_MSG(f.size == 88 && At(f, 24, 44) == 0xFF00FF00u, "twice the scale, twice the size");
}

}

int main()
{
   TestDotsBlinkAtTheDeviceCadence();
   TestRingPulseAndFlash();
   TestFramePixels();
   return ScorbitTest::Summary("status_light_raster_test");
}
