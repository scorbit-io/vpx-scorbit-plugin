// license:GPLv3+

#pragma once

namespace Scorbit
{

// The plugin-owned status light (docs/status-window.md). Prototype (SB-6106): it shows a fake
// status cycling through the indicator's states, to measure focus and visibility over VPX.
// corner is the StatusCorner setting: 0 off, 1 top left, 2 top right, 3 bottom left, 4 bottom right.
// API thread only, which is the main thread on macOS. A no-op elsewhere.
#ifdef __APPLE__
void ShowStatusWindow(int corner);
void HideStatusWindow();
#else
inline void ShowStatusWindow(int) { }
inline void HideStatusWindow() { }
#endif

}
