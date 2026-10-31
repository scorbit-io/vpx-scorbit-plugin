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
// from hidden to shown. API thread only, which is VPX's main thread. A no-op off macOS and Windows.
#if defined(__APPLE__) || defined(_WIN32)
bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash);
#else
inline bool UpdateStatusWindow(int, const LightView&, double, bool) { return false; }
#endif

}
