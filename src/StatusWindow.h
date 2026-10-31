// license:GPLv3+

#pragma once

#include "StatusLight.h"

namespace Scorbit
{

// The plugin-owned status light (docs/status-window.md), drawing a LightView.
// corner: 0 hidden, 1 top left, 2 top right, 3 bottom left, 4 bottom right (StatusCorner).
// opacity: 0.1 to 1 (StatusOpacity). flash: a new player claim, flashed once on the ring.
// Idempotent, so it is called on every main-thread tick and a setting change applies mid-game; it
// redraws only when the view changes. Hiding releases the window. Returns true when the light went
// from hidden to shown. API thread only, which is VPX's main thread. A no-op off macOS, Windows and X11.
#if defined(__APPLE__) || defined(_WIN32) || defined(SCORBIT_STATUS_X11)
bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash);
#else
inline bool UpdateStatusWindow(int, const LightView&, double, bool) { return false; }
#endif

// Why the last show was refused, as under Wayland, or nullptr. The notifications stay the surface.
#if defined(SCORBIT_STATUS_X11)
const char* StatusWindowUnavailable();
#else
inline const char* StatusWindowUnavailable() { return nullptr; }
#endif

}
