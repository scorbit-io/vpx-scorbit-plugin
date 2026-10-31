// license:GPLv3+

#pragma once

// What the status light shows (docs/status-window.md), decided apart from any window so it is
// tested on every platform. Two dots mirror the Scorbitron's two status LEDs, in their colours
// (full scale, before the device dims them) and their blink; a 30-segment ring stands for the tap
// pad, in a Scorbit style of its own; a glyph between the dots means no state is told by colour
// alone.

#include "StatusNotice.h"

#include <array>
#include <cstdint>
#include <string>

namespace Scorbit
{

struct LightRgb
{
   uint8_t r = 0, g = 0, b = 0;
   bool operator==(const LightRgb&) const = default;
};

// A dot blinks: lit for onMs, dark for offMs, over and over. An unlit dot is dark throughout.
struct LightDot
{
   bool lit = false;
   LightRgb rgb;
   uint16_t onMs = 0, offMs = 0;
   bool operator==(const LightDot&) const = default;
};

enum class LightRing : uint8_t
{
   Dark,
   Rainbow,   // pulsing, while all is well and no game is being scored
   Solid,     // ringRgb, for a problem the pad itself would show
};

enum class LightGlyph : uint8_t
{
   None,      // all is well
   Problem,   // "!": expected to work and does not
   Unknown,   // "?": a value this build does not know
};

struct LightView
{
   bool visible = false;   // false: no light at all, as when the daemon hides its indicator
   LightDot system;        // the Scorbitron's first LED: the device and its connection
   LightDot session;       // its second: the game being scored
   LightRing ring = LightRing::Dark;
   LightRgb ringRgb;
   LightGlyph glyph = LightGlyph::None;
   bool operator==(const LightView&) const = default;
};

namespace Light
{
inline constexpr LightRgb RED { 255, 0, 0 };
inline constexpr LightRgb GREEN { 0, 255, 0 };
inline constexpr LightRgb BLUE { 0, 0, 255 };
inline constexpr LightRgb AMBER { 255, 191, 0 };
inline constexpr LightRgb ORANGE { 255, 165, 0 };
inline constexpr LightRgb PURPLE { 255, 0, 255 };

// The first LED blinks 1200/100; the second is lit for 29 of 32 steps of 41 ms.
inline constexpr uint16_t SYSTEM_ON_MS = 1200, SYSTEM_OFF_MS = 100;
inline constexpr uint16_t SESSION_ON_MS = 29 * 41, SESSION_OFF_MS = 3 * 41;

inline constexpr size_t RING_SEGMENTS = 30;
}

// The light for the API thread's current record. A session still waiting for its first Status,
// or a loss younger than the notifications' grace, keeps what is shown, as the notifier does:
// a quick reconnect into the same state is not a transition.
LightView LightFor(const StatusRecord& record, StatusNotifier::Clock::time_point now, const LightView& shown);

// Spots a new claim, for the ring's flash: a slot whose player id appears or changes.
class ClaimWatch final
{
public:
   static constexpr size_t SLOTS = 9;
   using Ids = std::array<std::string, SLOTS>;

   // True when any slot holds a player id it did not hold last time. The first call after
   // Reset only learns the table, so a table that starts with players claimed does not flash.
   bool Update(const Ids& ids);
   void Reset() { m_known = false; }

private:
   Ids m_ids;
   bool m_known = false;
};

}
