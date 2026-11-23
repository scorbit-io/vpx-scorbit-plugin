// license:GPLv3+

#pragma once

#include "StatusBanner.h"
#include "StatusLight.h"

#include <string>

namespace Scorbit
{

// The plugin-owned status light (docs/status-window.md), drawing a LightView.
// corner: 0 hidden, 1 top left, 2 top right, 3 bottom left, 4 bottom right (StatusCorner).
// opacity: 0.1 to 1 (StatusOpacity). flash: a new player claim, flashed once on the ring.
// caption: the text shown beside the light while the cursor hovers over it (HoverTracker); empty
// shows none. Idempotent, so it is called on every main-thread tick and a setting change applies
// mid-game; it redraws only when the view changes. Hiding releases the window. Returns true when
// the light went from hidden to shown. API thread only, which is VPX's main thread. A no-op off
// macOS, Windows and X11.
#if defined(__APPLE__) || defined(_WIN32) || defined(SCORBIT_STATUS_X11)
bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash, const std::string& caption);
#else
inline bool UpdateStatusWindow(int, const LightView&, double, bool, const std::string&) { return false; }
#endif

// The status alert banner (docs/status-banner.md). API thread only. Shows the text for lifetime
// seconds and returns true, or returns false where no banner can show, so the caller posts
// VPX's notice instead. It is up only while VPX is the active application.
#if defined(__APPLE__) || defined(_WIN32)
bool ShowStatusBanner(const BannerText& text, double lifetimeSeconds);
void HideStatusBanner();
// Every tick: hides it once its lifetime is over, and while VPX is not active. release: at unload.
void TickStatusBanner(bool release);
#else
inline bool ShowStatusBanner(const BannerText&, double) { return false; }
inline void HideStatusBanner() { }
inline void TickStatusBanner(bool) { }
#endif

// Why no banner can show for this table, or nullptr: VPX's DirectX 9 build in exclusive
// fullscreen, which no window can cover (docs/status-banner.md). prefPath is VPX's preference
// folder as GetVpxInfo gives it, holding VPinballX.ini. Read once per table.
#if defined(_WIN32)
const char* StatusBannerBlocked(const std::string& prefPath);
#else
inline const char* StatusBannerBlocked(const std::string&) { return nullptr; }
#endif

// Why the last show was refused, as under Wayland, or nullptr. The notifications stay the surface.
#if defined(SCORBIT_STATUS_X11)
const char* StatusWindowUnavailable();
#else
inline const char* StatusWindowUnavailable() { return nullptr; }
#endif

}
