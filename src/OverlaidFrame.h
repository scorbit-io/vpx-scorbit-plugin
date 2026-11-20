// license:GPLv3+

#pragma once

// The overlaid identify frame (wire protocol 1.3): the plain identify frame with the overlay
// composited as the render path draws it, for the daemon's live feed. Composed on the socket
// worker from the published overlay record only; never from the render path's entries.

#include "OverlayCache.h"
#include "OverlayPlacement.h"
#include "OverlayState.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace Scorbit::Overlay
{

// An overlay value's displayed level at the platform's depth P, placed on the identify depth S.
// Integer rounding, half up, equals the wire definition's lround for these non-negative values.
inline uint8_t IdentifyShade(int displayed, int identify, uint8_t value)
{
   const int p = (2 * (value & 0x0F) * (displayed - 1) + 15) / 30;
   return static_cast<uint8_t>((2 * p * (identify - 1) + (displayed - 1)) / (2 * (displayed - 1)));
}

// The plain frame the overlaid one is built on. pixels holds width * height shade indices.
struct PlainFrame
{
   bool hasSource = false;
   uint32_t frameId = 0;
   uint32_t generation = 0;
   uint16_t width = 0;
   uint16_t height = 0;
   uint8_t shades = 0;
   const uint8_t* pixels = nullptr;
};

struct OverlaidFrame
{
   uint32_t seq = 0;
   bool showing = false;
   const std::vector<uint8_t>* pixels = nullptr;  // set when showing; valid until the next Compose
};

class OverlaidComposer final
{
public:
   using Clock = State::Clock;

   // Worker thread only. surface: the plugin has a display it composites on, so VPX shows overlays.
   OverlaidFrame Compose(const Cache& cache, Platform platform, bool surface, const PlainFrame& plain, Clock::time_point now)
   {
      cache.ReadIfChanged(m_recordSequence, m_record, m_scratch);

      const bool geometry = plain.hasSource && plain.pixels != nullptr && plain.width != 0 && plain.height != 0
         && (plain.shades == 4 || plain.shades == 16);
      Key key;
      key.record = m_recordSequence;
      if (geometry)
      {
         const Sweep sweep = m_record.SweepAt(now, plain.width);
         key.showing = surface && sweep.Visible();
         if (key.showing)
         {
            key.depth = Shades(platform, plain.shades == 16);
            // Only while an overlay shows do the plain frame and P change the overlaid one.
            key.sweep = sweep;
            key.frameId = plain.frameId;
            key.generation = plain.generation;
            key.width = plain.width;
            key.height = plain.height;
            key.shades = plain.shades;
         }
      }

      if (!m_last || !(*m_last == key))
      {
         m_last = key;
         m_seq++;
         if (key.showing)
            Draw(plain, key);
      }
      return { m_seq, key.showing, key.showing ? &m_pixels : nullptr };
   }

private:
   struct Key
   {
      uint64_t record = 0;
      int depth = 0;
      bool showing = false;
      Sweep sweep { };
      uint32_t frameId = 0;
      uint32_t generation = 0;
      uint16_t width = 0;
      uint16_t height = 0;
      uint8_t shades = 0;

      bool operator==(const Key&) const = default;
   };

   // As DmdOverlay::Composite on the controller's own display, in shade indices.
   void Draw(const PlainFrame& plain, const Key& key)
   {
      const size_t size = static_cast<size_t>(plain.width) * plain.height;
      m_pixels.assign(plain.pixels, plain.pixels + size);
      const Placement at = Place(plain.width, plain.height, m_record.width, m_record.height);
      const bool overlay = at.width != 0 && at.height != 0;
      if (!overlay && !key.sweep.Wiping())
         return;
      uint8_t shade[128];
      for (int v = 0; v < 128; v++)
         shade[v] = IdentifyShade(key.depth, plain.shades, static_cast<uint8_t>(v));
      for (uint32_t y = 0; y < plain.height; y++)
      {
         if (!(overlay && y >= at.dstY && y < at.dstY + at.height) && !key.sweep.Wiping())
            continue;
         for (uint32_t x = 0; x < plain.width; x++)
            if (const int v = ComposeDot(m_record, at, key.sweep, x, y); v >= 0)
               m_pixels[static_cast<size_t>(y) * plain.width + x] = shade[v];
      }
   }

   uint64_t m_recordSequence = 0;
   Record m_record;
   Record m_scratch;
   std::optional<Key> m_last;
   // Seeded from the clock, so a reloaded plugin does not replay a seq a daemon may still hold.
   uint32_t m_seq = static_cast<uint32_t>(Clock::now().time_since_epoch().count());
   std::vector<uint8_t> m_pixels;
};

}
