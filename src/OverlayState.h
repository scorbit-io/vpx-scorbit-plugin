// license:GPLv3+

#pragma once

// The DMD probe's overlay as plain state: one bitmap, an enable bit and a hide timer,
// with the probe firmware's rule for each (spb-firmwares RP2040 main.cpp and dmd.cpp),
// and the shade each overlay value shows at on each platform. The normative statement
// is scorbitd design.md, "Message layouts (protocol 1.0, slice 4)". Free of any Visual
// Pinball dependency so both are tested on their own; the socket path and the
// in-process Overlay message share it.

#include "OverlayPlacement.h"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Scorbit::Overlay
{

// Platforms whose probe decodes an overlay value differently. Spike 1 is not emulated
// by PinMAME, so it has no tables and no entry.
enum class Platform
{
   Unknown,
   Wpc,
   DataEast,  // DE 128x16 and 128x32, Whitestar of both eras, Sega 192x64
   Sam,
   Gts3,
   Capcom,
   AlvinG,
};

// PinMAME's GEN_* bits (src/wpc/gen.h), as GetMachineState:1 reports them in hardwareGen.
namespace Gen
{
inline constexpr uint64_t ALLWPC = 0x00000000000ffull;
inline constexpr uint64_t DEDMD16 = 0x0000000002000ull;
inline constexpr uint64_t DEDMD32 = 0x0000000004000ull;
inline constexpr uint64_t DEDMD64 = 0x0000000008000ull;
inline constexpr uint64_t ALLWS = 0x001c000000000ull;
inline constexpr uint64_t GTS3 = 0x0020000000000ull;
inline constexpr uint64_t SAM = 0x0100000000000ull;
inline constexpr uint64_t ALVG = 0x0200000000000ull;
inline constexpr uint64_t ALVG_DMD2 = 0x0400000000000ull;
inline constexpr uint64_t CAPCOM = 0x20000000000000ull;
}

inline Platform PlatformOf(uint64_t hardwareGen)
{
   if (hardwareGen & Gen::ALLWPC)
      return Platform::Wpc;
   if (hardwareGen & (Gen::DEDMD16 | Gen::DEDMD32 | Gen::DEDMD64 | Gen::ALLWS))
      return Platform::DataEast;
   if (hardwareGen & Gen::SAM)
      return Platform::Sam;
   if (hardwareGen & Gen::GTS3)
      return Platform::Gts3;
   if (hardwareGen & Gen::CAPCOM)
      return Platform::Capcom;
   if (hardwareGen & (Gen::ALVG | Gen::ALVG_DMD2))
      return Platform::AlvinG;
   return Platform::Unknown;
}

inline const char* PlatformName(Platform p)
{
   switch (p)
   {
   case Platform::Wpc: return "WPC";
   case Platform::DataEast: return "Data East / Whitestar / Sega";
   case Platform::Sam: return "SAM";
   case Platform::Gts3: return "Gottlieb System 3";
   case Platform::Capcom: return "Capcom";
   case Platform::AlvinG: return "Alvin G";
   default: return "unknown";
   }
}

// SAM writes a 4-bit value per dot and the board makes the signals, so an overlay value
// is drawn exactly as PinMAME draws a game pixel of that value (src/wpc/core.c,
// CORE_DMD_PWM_PREINTEGRATED_SAM). 8 to 11 are never written by games and map darker.
inline constexpr float SAM_LUMINANCE[16] = { 0.f, 1.f / 12, 2.f / 12, 3.f / 12, 4.f / 12, 5.f / 12, 6.f / 12, 7.f / 12,
   5.f / 12, 6.f / 12, 7.f / 12, 8.f / 12, 9.f / 12, 10.f / 12, 11.f / 12, 1.f };

// The luminance an overlay value shows at: the probe's own decode of it as shade over
// the platform's top shade. Bits a platform does not use are ignored. Unknown falls
// back to WPC's rule, or to the low four bits on a 16-shade display.
inline float Luminance(Platform platform, uint8_t value, bool sixteenShades)
{
   const auto lit = [value](unsigned mask) { return static_cast<float>(std::popcount(static_cast<unsigned>(value) & mask)); };
   switch (platform)
   {
   case Platform::Wpc: return lit(0x07) / 3.f;
   case Platform::DataEast: return static_cast<float>(value & 0x03) / 3.f;
   case Platform::Sam: return SAM_LUMINANCE[value & 0x0F];
   case Platform::Gts3: return lit(0x3F) / 6.f;
   // Alvin G is a correctable guess: PinMAME's four equal-weight planes.
   case Platform::Capcom:
   case Platform::AlvinG: return lit(0x0F) / 4.f;
   default: return sixteenShades ? static_cast<float>(value & 0x0F) / 15.f : lit(0x07) / 3.f;
   }
}

// Not TRANSPARENT: wingdi.h defines that as a macro.
inline constexpr uint8_t TRANSPARENT_BIT = 0x80;

class State final
{
public:
   using Clock = std::chrono::steady_clock;

   enum class Upload
   {
      Stored,
      Ignored,    // outside the size bounds: acknowledged, previous bitmap kept
      Malformed,  // shorter than its own width and height say
   };

   // The 0x23 payload: width, height, then width * height pixels, row major. Bytes past
   // those are ignored, as the firmware ignores them. Replaces a visible bitmap at once.
   Upload Store(const uint8_t* payload, size_t size)
   {
      const Upload check = Check(payload, size);
      if (check != Upload::Stored)
         return check;
      m_width = payload[0];
      m_height = payload[1];
      m_pixels.assign(payload + 2, payload + 2 + static_cast<size_t>(m_width) * m_height);
      return Upload::Stored;
   }

   // What Store would do, without storing.
   static Upload Check(const uint8_t* payload, size_t size)
   {
      if (payload == nullptr || size < 2 || size - 2 < static_cast<size_t>(payload[0]) * payload[1])
         return Upload::Malformed;
      return Accepted(payload[0], payload[1]) ? Upload::Stored : Upload::Ignored;
   }

   // The 0x27 control. Enabling arms the bit and restarts the timer, 0 meaning until
   // hidden, whether or not a bitmap has arrived yet: one that follows inside the
   // window shows at once. On expiry the overlay hides itself.
   void Control(bool enable, uint32_t durationMs, Clock::time_point now)
   {
      m_enabled = enable;
      m_untilHidden = durationMs == 0;
      m_hideAt = now + std::chrono::milliseconds(durationMs);
   }

   // Session end: hidden, with no bitmap and no timer left behind.
   void Clear()
   {
      m_enabled = false;
      m_untilHidden = false;
      m_hideAt = { };
      m_width = m_height = 0;
      m_pixels.clear();
   }

   bool Visible(Clock::time_point now) const { return m_enabled && !m_pixels.empty() && (m_untilHidden || now < m_hideAt); }

   bool Enabled() const { return m_enabled; }
   bool UntilHidden() const { return m_untilHidden; }
   Clock::time_point HideAt() const { return m_hideAt; }
   uint32_t Width() const { return m_width; }
   uint32_t Height() const { return m_height; }
   const std::vector<uint8_t>& Pixels() const { return m_pixels; }

private:
   bool m_enabled = false;
   bool m_untilHidden = false;
   Clock::time_point m_hideAt { };
   uint32_t m_width = 0;
   uint32_t m_height = 0;
   std::vector<uint8_t> m_pixels;
};

}
