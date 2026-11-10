// license:GPLv3+

#pragma once

// The transition the DMD probe firmware plays when an overlay shows or hides: a band that sweeps
// left to right across the display, revealing the overlay behind it or masking it again. Timing,
// interruptions and band shape follow the firmware's band wipe; the band's random dots match it in
// distribution, not bit for bit.
//
// Everything here is a pure function of time. A wipe is stored as where and when it started, and
// each reader derives the step it is at, so nothing advances it and every render thread agrees.
// Stepping by elapsed time is also the firmware's catch-up: a late frame lands on the right step.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace Scorbit::Overlay
{

// The firmware's numbering, so a setting carries over. Only sparkle is drawn so far.
enum class Effect : uint8_t
{
   Sparkle = 1,
};

inline constexpr uint32_t BAND_MAX_WIDTH = 16;

// The probe's persistent transition settings, at the firmware's defaults.
struct TransitionSettings
{
   bool on = true;
   Effect effect = Effect::Sparkle;
   uint32_t paceMs = 10;   // per display column
   uint32_t width = 8;     // band columns, 0 disabling the transition

   bool operator==(const TransitionSettings&) const = default;
};

// Whether a control message transitions: its force and disable bits against the setting. A band
// of width 0 never does.
inline bool Transitions(const TransitionSettings& s, bool force, bool disable)
{
   if (s.width == 0)
      return false;
   return (s.on && !disable) || (!s.on && force);
}

enum class Reveal : uint8_t
{
   Off,
   Full,
   Show,   // a wipe revealing the overlay
   Hide,   // a wipe masking it
};

using Ticks = int64_t;   // steady_clock ticks

// The reveal as it was last set: a settled state, or a wipe with where and when it started. Its
// effect is latched at the start, as the firmware latches its type; so are pace and width, which
// the firmware reads live, but which here change only when the plugin loads.
struct Wipe
{
   Reveal reveal = Reveal::Off;
   Ticks startAt = 0;
   int32_t limit = 0;       // Hide: overlay columns still shown are those below it
   uint32_t seed = 0;       // a new one for every wipe
   uint32_t paceMs = 10;
   uint32_t width = 8;
   Effect effect = Effect::Sparkle;
};

// Far beyond any display, and far from overflow.
inline constexpr int32_t NO_LIMIT = 1 << 20;

inline Ticks TicksOf(std::chrono::steady_clock::time_point t) { return t.time_since_epoch().count(); }

inline Ticks MsTicks(uint32_t ms)
{
   return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(ms)).count();
}

// The band's left edge: from just off the left, one column a pace. Capped at NO_LIMIT.
inline int32_t Frontier(const Wipe& w, Ticks now)
{
   const Ticks pace = MsTicks(std::max<uint32_t>(w.paceMs, 1));
   const Ticks steps = now <= w.startAt ? 0 : (now - w.startAt) / pace;
   return static_cast<int32_t>(std::min<Ticks>(steps - static_cast<Ticks>(w.width), NO_LIMIT));
}

inline Wipe StartWipe(Reveal reveal, Ticks at, int32_t limit, const TransitionSettings& s, uint32_t previousSeed)
{
   return { reveal, at, limit, previousSeed + 1, std::max<uint32_t>(s.paceMs, 1), std::min(s.width, BAND_MAX_WIDTH), s.effect };
}

// Everything that decides what an overlay shows at a time, as the writer last set it.
struct Timeline
{
   bool enabled = false;      // the 0x27 enable bit as last written
   bool untilHidden = false;
   Ticks hideAt = 0;          // the hold timer, which clears the enable bit
   bool transition = false;   // whether the last control message transitions
   TransitionSettings settings;
   Wipe wipe;
};

// The enable bit clears at `at`: the overlay hides, with a wipe when the last control transitions.
// A hide that interrupts a show keeps only the columns it had revealed, and drops the overlay at
// once if it had revealed none.
inline Timeline BeginHide(Timeline t, Ticks at)
{
   t.enabled = false;
   if (!t.transition)
   {
      t.wipe.reveal = Reveal::Off;
      return t;
   }
   switch (t.wipe.reveal)
   {
   case Reveal::Full: t.wipe = StartWipe(Reveal::Hide, at, NO_LIMIT, t.settings, t.wipe.seed); break;
   case Reveal::Show:
   {
      const int32_t limit = Frontier(t.wipe, at);
      if (limit <= 0)
         t.wipe.reveal = Reveal::Off;
      else
         t.wipe = StartWipe(Reveal::Hide, at, limit, t.settings, t.wipe.seed);
      break;
   }
   default: break;
   }
   return t;
}

// The timeline with the hold timer applied: once it has run out, the hide it began has begun.
inline Timeline Settle(const Timeline& t, Ticks now)
{
   return t.enabled && !t.untilHidden && now >= t.hideAt ? BeginHide(t, t.hideAt) : t;
}

// What a reader draws at a time on a display `displayWidth` base columns wide.
struct Sweep
{
   Reveal reveal = Reveal::Off;   // Off, Full, or a wipe in progress
   int32_t frontier = 0;          // the band's left edge
   int32_t width = 0;             // the band's width
   int32_t limit = 0;             // Hide: overlay columns at or past it are not drawn
   uint32_t seed = 0;
   Effect effect = Effect::Sparkle;

   bool Visible() const { return reveal != Reveal::Off; }
   bool Wiping() const { return reveal == Reveal::Show || reveal == Reveal::Hide; }
   bool operator==(const Sweep&) const = default;

   // Show: the overlay left of the band. Hide: right of it, below the limit.
   bool ShowsOverlay(int32_t column) const
   {
      switch (reveal)
      {
      case Reveal::Full: return true;
      case Reveal::Show: return column < frontier;
      case Reveal::Hide: return column >= frontier + width && column < limit;
      default: return false;
      }
   }

   // The band column at a display column, its head (0) facing the overlay, or -1 outside the band.
   int32_t BandColumn(int32_t column) const
   {
      if (!Wiping() || column < frontier || column >= frontier + width)
         return -1;
      const int32_t x = column - frontier;
      return reveal == Reveal::Hide ? width - 1 - x : x;
   }
};

inline Sweep SweepAt(const Timeline& timeline, Ticks now, uint32_t displayWidth)
{
   const Timeline t = Settle(timeline, now);
   const Wipe& w = t.wipe;
   Sweep s;
   switch (w.reveal)
   {
   case Reveal::Off: return s;
   case Reveal::Full: s.reveal = Reveal::Full; return s;
   default: break;
   }
   const int32_t display = static_cast<int32_t>(std::min<uint32_t>(displayWidth, NO_LIMIT));
   const int32_t end = w.reveal == Reveal::Show ? display : std::min(w.limit, display);
   s.frontier = Frontier(w, now);
   if (s.frontier >= end)
   {
      s.reveal = w.reveal == Reveal::Show ? Reveal::Full : Reveal::Off;
      s.frontier = 0;
      return s;
   }
   s.reveal = w.reveal;
   s.width = static_cast<int32_t>(w.width);
   s.limit = end;
   s.seed = w.seed;
   s.effect = w.effect;
   return s;
}

// With no bitmap a settled overlay draws nothing, but a wipe's band still crosses the game.
inline Sweep WithBitmap(const Sweep& s, bool bitmap)
{
   return bitmap || s.Wiping() ? s : Sweep { };
}

// --- Band contents ---

// The firmware's sparkle: the chance, out of 256, that a dot in a column is lit, head first.
inline constexpr uint8_t SPARKLE_DENSITY[BAND_MAX_WIDTH] = { 124, 121, 125, 98, 92, 80, 68, 60, 48, 41, 30, 32, 15, 12, 6, 4 };

// One well-mixed 32-bit value per dot and step (Murmur3's finaliser).
inline uint32_t DotHash(uint32_t seed, int32_t frontier, int32_t x, int32_t y)
{
   uint32_t h = seed * 0x9E3779B9u ^ static_cast<uint32_t>(frontier) * 0x85EBCA6Bu ^ static_cast<uint32_t>(x) * 0xC2B2AE35u
      ^ static_cast<uint32_t>(y) * 0x27D4EB2Fu;
   h ^= h >> 16;
   h *= 0x85EBCA6Bu;
   h ^= h >> 13;
   h *= 0xC2B2AE35u;
   h ^= h >> 16;
   return h;
}

// The brightness, 0 to 15, of the band dot at band column x (0, the head, faces the overlay) and
// row y in the sweep's current step; 0 is unlit, and shows the game. Drawn afresh every step.
inline uint8_t BandDot(const Sweep& s, int32_t x, int32_t y)
{
   const uint32_t r = DotHash(s.seed, s.frontier, x, y);
   switch (s.effect)
   {
   case Effect::Sparkle:
   default:
   {
      const int32_t column = s.width <= 1 ? 0 : x * static_cast<int32_t>(BAND_MAX_WIDTH - 1) / (s.width - 1);
      if ((r & 0xFF) >= SPARKLE_DENSITY[column])
         return 0;
      const uint32_t pick = (r >> 8) & 3;
      return pick == 0 ? 9 : pick == 1 ? 12 : 15;
   }
   }
}

}
