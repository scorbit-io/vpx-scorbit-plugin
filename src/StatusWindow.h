// license:GPLv3+

#pragma once

namespace Scorbit
{

// The plugin-owned status light (docs/status-window.md). Prototype (SB-6106): it shows a fake
// status cycling through the indicator's states, to measure focus and visibility over VPX.
// corner: 0 hidden, 1 top left, 2 top right, 3 bottom left, 4 bottom right (StatusCorner).
// Idempotent, so it is called on every main-thread tick and a setting change applies mid-game;
// hiding releases the window and its timer. Returns true when the light went from hidden to shown.
// API thread only, which is the main thread on macOS. A no-op elsewhere.
#ifdef __APPLE__
bool UpdateStatusWindow(int corner);
#else
inline bool UpdateStatusWindow(int) { return false; }
#endif

}
