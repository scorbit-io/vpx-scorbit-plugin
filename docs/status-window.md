# A plugin-owned status window (design, SB-6004)

Status: design. Nothing here is implemented yet. A macOS prototype comes next, and then one
implementation ticket per platform.

## Why

The Scorbit status indicator (SPEC-0002 REQ-6.6) is a small colour light in the Scorbitron's
vocabulary. It must never cover game content and must work on tables with no DMD.

Inside VPX's renderer a plugin cannot draw one without owning a whole ancillary window
(REQ-6.8). VPX notifications are text only; they are the interim surface, with `[OK]`/`[!]`/`[?]`
markers and periodic refresh (SB-6003, SB-6006). So the plugin draws the indicator in a small
window of its own, outside VPX's renderer. VPX needs no change for it.

## The one hard rule: never take focus

VPX plays only while its playfield window holds SDL keyboard focus. `Player::IsPlaying` checks
`m_playfieldWnd->IsFocused()`, which compares the window to `SDL_GetKeyboardFocus()` (vpinball
9a8f24cb6, `src/core/player.h:88`, `src/renderer/Window.cpp:371-375`). Losing it pauses the table
and logs `Playfield window lost focus` (`player.cpp:1546-1553`).

VPX itself has the same problem with its own backglass, score-view and topper windows. On macOS
and every other non-Windows, non-X11 driver it creates them `SDL_WINDOW_NOT_FOCUSABLE`, because
showing them would otherwise "steal focus from the playfield and pause the table"
(`Window.cpp:219-227`). The status window follows the same rule, with each platform's native
equivalent.

Acceptance for every platform: a five-minute session with the window visible logs no
`Playfield window lost focus`.

## The window

The same on every platform:

- Created and driven on the plugin API thread. On macOS that is the main thread, which AppKit
  requires; the plugin schedules there with `MsgPluginAPI::RunOnMainThread`.
- Transparent, borderless, click-through, and sized to the indicator (about 24 to 32 points, with
  room for the glyph).
- Redrawn only when the status changes. A pulse is an OS-side animation (Core Animation, or a
  layered-window timer), never per-frame work on VPX's render path.
- Placed on the display the playfield window is on, in a corner chosen by a new plugin setting
  (`StatusCorner`, default top right). `ShowStatus = 0` hides it, along with the notifications.
- The daemon's `Status` (slice 4) drives it: no wire change.
- Drawing: the Scorbitron's colours and pulses (REQ-6.2) plus the REQ-6.5 glyph, so no state is
  told by colour alone. The support text stays in the notification, which keeps posting
  `[!]` states (SB-6006).
- If the window cannot be created (for example on Wayland), the notifications remain the
  surface, exactly as today.

### macOS

- An `NSPanel` with `NSWindowStyleMaskNonactivatingPanel | NSWindowStyleMaskBorderless`.
  `canBecomeKeyWindow` and `canBecomeMainWindow` return `NO`, `ignoresMouseEvents = YES`,
  `hidesOnDeactivate = NO`, `opaque = NO` with a clear background.
- `collectionBehavior` includes `.canJoinAllSpaces`, `.fullScreenAuxiliary` and `.stationary`.
  VPX's fullscreen is SDL's, which on macOS puts the window in its own Space by default; only a
  full-screen-auxiliary window may show there.
- Level above the playfield window. `NSStatusWindowLevel` is the first candidate; the prototype
  measures whether it stays above VPX in all three window modes.
- Ordered with `orderFrontRegardless`, never `makeKeyAndOrderFront`.

### Windows

- A layered window: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
  WS_EX_TOOLWINDOW`, `WS_POPUP`, shown with `SW_SHOWNOACTIVATE`, drawn with
  `UpdateLayeredWindow` (per-pixel alpha).
- Owned by no window, so that clicking VPX never reorders it below.

### Linux

- X11: an override-redirect window with an empty input shape (XShape), so it takes no input and
  no focus.
- Wayland: clients cannot position windows or keep them on top, short of the layer-shell
  protocol, which SDL and most compositors used for VPX don't offer. **Unsupported**: the
  notification stays the surface. The plugin detects the driver the way VPX does
  (`SDL_GetCurrentVideoDriver`, `Window.cpp:208-220`).

## Risks, and how the prototype measures them

| Risk | Platform | Measurement |
|---|---|---|
| The window takes focus and pauses the table | all | 5-minute session, window visible and the state changing; `vpinball.log` has no `Playfield window lost focus` |
| Not visible over VPX's fullscreen | macOS | windowed, borderless fullscreen and exclusive fullscreen (`Window.cpp:89-121`, 257-262), each with a cropped screenshot of the corner |
| Hidden by exclusive fullscreen | Windows | VPX's `ExclusiveFullscreen` on BGFX D3D11/12. A topmost layered window over an exclusive swap chain is typically not shown; if so, that mode falls back to notifications, and the note records it |
| Adds latency in borderless fullscreen | Windows | a window overlapping a fullscreen swap chain can take it off independent flip, adding a frame of latency. Compare `PresentMon` with the window shown and hidden |
| Multi-screen cabinets | all | the window follows the playfield's display, not the backglass's; check on a two-display setup |
| Cost on the render path | all | none by design: frame times with and without the window, from VPX's own performance overlay |

## Order

1. This note.
2. macOS prototype on the rig: the window as above driven by a fake status, then the focus and
   fullscreen rows of the risk table, each a live VPX session with the screen unlocked.
3. One implementation ticket per platform: macOS, Windows, then X11.
4. REQ-6.6 already names this surface as plugin-owned rather than an upstream VPX event.
