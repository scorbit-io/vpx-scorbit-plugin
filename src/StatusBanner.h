// license:GPLv3+

#pragma once

// The status alert banner's content and size (docs/status-banner.md), the same on every platform.

#include <string>
#include <string_view>

namespace Scorbit
{

enum class BannerIcon
{
   Info,     // [OK]
   Problem,  // [!]
   Unknown,  // [?]
};

struct BannerText
{
   BannerIcon icon = BannerIcon::Info;
   std::string text;

   bool operator==(const BannerText&) const = default;
};

// A notice's leading marker becomes the icon, and the rest is the text. A notice without one
// is shown as information, whole.
inline BannerText BannerFor(const std::string& notice)
{
   static constexpr struct { std::string_view marker; BannerIcon icon; } MARKERS[] = {
      { "[!]", BannerIcon::Problem }, { "[OK]", BannerIcon::Info }, { "[?]", BannerIcon::Unknown } };
   for (const auto& m : MARKERS)
      if (notice.starts_with(m.marker))
      {
         const size_t start = notice.find_first_not_of(' ', m.marker.size());
         return { m.icon, start == std::string::npos ? std::string() : notice.substr(start) };
      }
   return { BannerIcon::Info, notice };
}

// The text's character height in device-independent units (macOS points, or pixels at 96 DPI):
// 1.5 times VPX's own 13, chosen live (a 1/30-of-the-display size read as far too big).
inline constexpr double BANNER_TEXT_HEIGHT = 20;

inline constexpr double BANNER_MAX_WIDTH = 0.6;  // of the playfield window's width
inline constexpr int BANNER_MAX_LINES = 3;       // a longer text is cut with an ellipsis
inline constexpr double BANNER_TOP = 0.25;       // the banner's top, down from the playfield window's top

}
