// license:GPLv3+

#include "StatusLightRaster.h"

#include <algorithm>
#include <cmath>

namespace Scorbit
{

namespace
{

constexpr double PI = 3.14159265358979323846;

bool Lit(const LightDot& dot, double seconds)
{
   if (!dot.lit || dot.onMs + dot.offMs == 0)
      return false;
   const double period = (dot.onMs + dot.offMs) / 1000.0;
   return std::fmod(seconds, period) < dot.onMs / 1000.0;
}

uint8_t Byte(double v)
{
   return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

struct Rgba
{
   double r = 0, g = 0, b = 0, a = 0; // premultiplied, 0 to 1
};

// Source over destination, both premultiplied.
void Over(Rgba& dst, double r, double g, double b, double alpha)
{
   dst.r = r * alpha + dst.r * (1 - alpha);
   dst.g = g * alpha + dst.g * (1 - alpha);
   dst.b = b * alpha + dst.b * (1 - alpha);
   dst.a = alpha + dst.a * (1 - alpha);
}

void Hue(double h, double& r, double& g, double& b)
{
   // Full saturation and brightness, as the macOS ring's colorWithHue.
   const double x = h * 6;
   const int sector = static_cast<int>(x) % 6;
   const double f = x - std::floor(x);
   switch (sector)
   {
   case 0: r = 1; g = f; b = 0; break;
   case 1: r = 1 - f; g = 1; b = 0; break;
   case 2: r = 0; g = 1; b = f; break;
   case 3: r = 0; g = 1 - f; b = 1; break;
   case 4: r = f; g = 0; b = 1; break;
   default: r = 1; g = 0; b = 1 - f; break;
   }
}

double Circle(double px, double py, double cx, double cy, double radius)
{
   return std::hypot(px - cx, py - cy) - radius;
}

double Capsule(double px, double py, double ax, double ay, double bx, double by, double radius)
{
   const double vx = bx - ax, vy = by - ay;
   const double t = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0, 1.0);
   return std::hypot(px - ax - t * vx, py - ay - t * vy) - radius;
}

// The glyph between the dots, in points about the light's centre (y down): a bold "!" or "?".
double Glyph(LightGlyph glyph, double x, double y)
{
   const double dot = Circle(x, y, 0, 4, 1.3);
   if (glyph == LightGlyph::Problem)
      return std::min(Capsule(x, y, 0, -5, 0, 1, 1.1), dot);
   // "?": an arc from the left, over the top, down to the right, then a stem to the centre.
   constexpr double CY = -2.6, R = 2.5, W = 1.0, END = 60 * PI / 180;
   const double angle = std::atan2(y - CY, x);
   const double ex = R * std::cos(END), ey = CY + R * std::sin(END);
   const double arc = (angle <= END) ? std::fabs(std::hypot(x, y - CY) - R) - W
                                     : std::min(std::hypot(x + R, y - CY), std::hypot(x - ex, y - ey)) - W;
   return std::min({ arc, Capsule(x, y, ex, ey, 0, 1.2, W), dot });
}

}

LightPhase PhaseAt(const LightView& view, double seconds, double flashSeconds)
{
   using namespace LightGeometry;
   LightPhase phase;
   phase.systemLit = Lit(view.system, seconds);
   phase.sessionLit = Lit(view.session, seconds);
   if (view.ring == LightRing::Solid)
      phase.ring = 255;
   else if (view.ring == LightRing::Rainbow)
   {
      // Linear from full to RING_PULSE_LOW and back, as Core Animation's autoreversing pulse.
      const double t = std::fmod(seconds, 2 * RING_PULSE_S) / RING_PULSE_S;
      phase.ring = Byte(1 - (1 - RING_PULSE_LOW) * (t < 1 ? t : 2 - t));
   }
   if (flashSeconds >= 0 && flashSeconds < FLASH_S)
   {
      const double f = flashSeconds / FLASH_S;
      phase.flash = Byte(f < 0.3 ? f / 0.3 : (1 - f) / 0.7);
   }
   return phase;
}

void Rasterize(const LightView& view, const LightPhase& phase, double scale, LightFrame& out)
{
   using namespace LightGeometry;
   scale = std::max(scale, 0.25);
   out.size = std::max(1, static_cast<int>(std::lround(DIAMETER * scale)));
   out.pixels.assign(static_cast<size_t>(out.size) * out.size, 0);
   const double c = DIAMETER / 2;
   const double ringAlpha = view.ring == LightRing::Dark ? 0 : phase.ring / 255.0, flashAlpha = phase.flash / 255.0;

   double segR[Light::RING_SEGMENTS], segG[Light::RING_SEGMENTS], segB[Light::RING_SEGMENTS];
   double segX[Light::RING_SEGMENTS], segY[Light::RING_SEGMENTS];
   for (size_t i = 0; i < Light::RING_SEGMENTS; i++)
   {
      // From the top, clockwise, like the pad's LED 0.
      const double a = PI / 2 - 2 * PI * static_cast<double>(i) / Light::RING_SEGMENTS;
      segX[i] = c + RING_RADIUS * std::cos(a);
      segY[i] = c - RING_RADIUS * std::sin(a);
      if (view.ring == LightRing::Solid)
      {
         segR[i] = view.ringRgb.r / 255.0;
         segG[i] = view.ringRgb.g / 255.0;
         segB[i] = view.ringRgb.b / 255.0;
      }
      else
         Hue(static_cast<double>(i) / Light::RING_SEGMENTS, segR[i], segG[i], segB[i]);
   }

   // Coverage of a shape whose signed distance (in points) is d: one pixel of anti-aliasing.
   const auto cover = [scale](double d) { return std::clamp(0.5 - d * scale, 0.0, 1.0); };
   const auto dot = [&](Rgba& px, const LightDot& d, bool lit, double x, double y, double cx)
   {
      if (!d.lit || !lit)
         return;
      const double k = cover(Circle(x, y, cx, c, DOT / 2));
      if (k > 0)
         Over(px, d.rgb.r / 255.0, d.rgb.g / 255.0, d.rgb.b / 255.0, k);
   };

   for (int py = 0; py < out.size; py++)
      for (int px = 0; px < out.size; px++)
      {
         const double x = (px + 0.5) / scale, y = (py + 0.5) / scale;
         Rgba p;
         const double disc = cover(Circle(x, y, c, c, c));
         if (disc <= 0)
            continue;
         Over(p, 0, 0, 0, disc * BACKGROUND_ALPHA / 255.0);
         if (ringAlpha > 0 && std::fabs(std::hypot(x - c, y - c) - RING_RADIUS) < SEGMENT)
            for (size_t i = 0; i < Light::RING_SEGMENTS; i++)
               if (const double k = cover(Circle(x, y, segX[i], segY[i], SEGMENT / 2)); k > 0)
                  Over(p, segR[i], segG[i], segB[i], k * ringAlpha);
         if (flashAlpha > 0)
            if (const double k = cover(std::fabs(std::hypot(x - c, y - c) - RING_RADIUS) - SEGMENT / 2); k > 0)
               Over(p, 1, 1, 1, k * flashAlpha);
         dot(p, view.system, phase.systemLit, x, y, c - DOT_OFFSET);
         dot(p, view.session, phase.sessionLit, x, y, c + DOT_OFFSET);
         if (view.glyph != LightGlyph::None)
            if (const double k = cover(Glyph(view.glyph, x - c, y - c)); k > 0)
               Over(p, 1, 1, 1, k);
         out.pixels[static_cast<size_t>(py) * out.size + px] = (static_cast<uint32_t>(Byte(p.a)) << 24)
            | (static_cast<uint32_t>(Byte(p.r)) << 16) | (static_cast<uint32_t>(Byte(p.g)) << 8) | Byte(p.b);
      }
}

}
