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
- Placement, against "never cover game content": no corner of a full-bleed fullscreen table is
  guaranteed free, so the plugin cannot promise that on its own.
  - It first looks for space the table does not draw in: on the playfield's display, outside the
    playfield window when that window does not fill the display (windowed mode, or a portrait
    table pillarboxed on a landscape screen).
  - **Failing that, by default there is no window**, and the notifications stay the surface. Lower
    opacity does not stop a light covering game content, so the default keeps the requirement.
  - A user who knows a corner their tables leave empty can opt in with a new plugin setting,
    `StatusCorner` (unset by default; top left, top right, bottom left or bottom right), and
    `StatusOpacity` for how strongly it draws there.
  - `ShowStatus = 0` hides the window and the notifications, as today.
- Shown only while VPX is the active application and a table is playing. Hidden when the user
  switches away, and shown again on return, so it never sits on top of another application.
- Driven by the plugin's whole local status record, not the daemon's `Status` alone. The daemon
  cannot report a connection that never started, or one that dropped after being healthy, so the
  light also shows the plugin's own no-session state, after the same 5 s grace the
  notifications use (`StatusRecord`, `StatusNotifier::NO_SESSION_AFTER`). No wire change.
- Drawing: the Scorbitron's colours and pulses (REQ-6.2) plus the REQ-6.5 glyph, so no state is
  told by colour alone. The support text stays in the notification, which keeps posting
  `[!]` states (SB-6006).
- If the window cannot be created (for example on Wayland), the notifications remain the
  surface, exactly as today.

### macOS

- An `NSPanel` with `NSWindowStyleMaskNonactivatingPanel | NSWindowStyleMaskBorderless`.
  `canBecomeKeyWindow` and `canBecomeMainWindow` return `NO`, `ignoresMouseEvents = YES`,
  `hidesOnDeactivate = YES`, so it goes when VPX stops being the active application and comes back
  when it returns; `opaque = NO`, with a clear background.
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
- Hidden while the foreground window belongs to another process. A `SetWinEventHook` on
  `EVENT_SYSTEM_FOREGROUND`, out of context, tells the plugin when the foreground changes;
  the plugin shows the window again when VPX's process regains it.

### Linux

- X11: an override-redirect window with an empty input shape (XShape), so it takes no input and
  no focus.
  - The window manager does not stack an override-redirect window, so nothing keeps it above the
    playfield by itself. The plugin restacks it: `XRaiseWindow` after the playfield is mapped or
    raised (seen from `ConfigureNotify` on the playfield's parent), and on each state change.
    Raising an override-redirect window moves no focus.
  - It is unmapped while another application's window is focused (`_NET_ACTIVE_WINDOW`), as on
    the other platforms.
  - A compositing window manager may unredirect a fullscreen playfield, which can hide any window
    above it; the prototype's risk table covers that.
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
| Covers game content | all | windowed and pillarboxed tables: the window sits outside the playfield; full-bleed tables: no window by default; with `StatusCorner` set, screenshots of that corner on a few tables |
| Stays on top of other apps | all | switch away from VPX mid-game and back; the window hides and returns |
| Hidden under the playfield | X11 | windowed and fullscreen, with and without a compositor; the window stays above after the playfield is raised |
| Multi-screen cabinets | all | the window follows the playfield's display, not the backglass's; check on a two-display setup |
| Cost on the render path | all | none by design: frame times with and without the window, from VPX's own performance overlay |

## Order

1. This note.
2. macOS prototype on the rig: the window as above driven by a fake status, then the focus and
   fullscreen rows of the risk table, each a live VPX session with the screen unlocked.
3. One implementation ticket per platform: macOS, Windows, then X11.
4. REQ-6.6 already names this surface as plugin-owned rather than an upstream VPX event.
