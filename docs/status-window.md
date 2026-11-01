# A plugin-owned status window (design, SB-6004)

Status: implemented on macOS, Windows and X11 (see "What the light shows"). Wayland keeps the
notifications. The measurements are under "Prototype results, macOS", "Results, Windows" and
"Results, X11".

## Why

The Scorbit status indicator is a small colour light in the Scorbitron's
vocabulary. It must never cover game content and must work on tables with no DMD.

Inside VPX's renderer a plugin cannot draw one without owning a whole ancillary window.
VPX notifications are text only; they are the interim surface, with `[OK]`/`[!]`/`[?]`
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
- Drawing: the Scorbitron's colours and pulses plus a glyph, so no state is
  told by colour alone. The support text stays in the notification, which keeps posting
  `[!]` states (SB-6006).
- If the window cannot be created (for example on Wayland), the notifications remain the
  surface, exactly as today.

### macOS

- A plain `NSPanel` with `NSWindowStyleMaskNonactivatingPanel | NSWindowStyleMaskBorderless`,
  never a subclass: a borderless panel already refuses key and main. `ignoresMouseEvents = YES`,
  `opaque = NO`, with a clear background.
- The playfield's screen is the key window's. VPX plays only while the playfield holds keyboard
  focus and makes its other windows unfocusable on macOS, so, while VPX is active, the key window is
  the playfield; window size says nothing (a backglass can be the larger window). The plugin API
  has no window handle to ask instead.
- It shows while VPX reports a game (`OnGameStart` to `OnGameEnd`), so EM and authored tables get
  it too, and re-reads `ShowStatus` and `StatusCorner` on each main-thread tick.
- It follows `NSApplicationDidBecomeActive` / `DidResignActive` itself, so it goes when VPX stops
  being the active application and comes back when it returns. Not `hidesOnDeactivate`: see the
  prototype results.
- Nothing Core Animation or AppKit can still hold after unload may live in the plugin: no class of
  ours, no string or number literal as an animation key or value, and the last transaction flushed
  (`[CATransaction flush]`) before the plugin returns from unload.
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

- X11: an override-redirect window with an empty input shape (SHAPE 1.1), so the window manager
  never manages or focuses it and it takes no input. Its `WM_CLASS` is `scorbit-status-light`, so a
  compositor can be told to leave it alone (no shadow or fade).
  - Drawn through xcb on a connection of the plugin's own, with the libraries loaded at run time:
    the plugin still loads where there is no X. xcb reports an error with its request, where Xlib
    would call a process-wide handler that SDL also swaps and whose default exits.
  - A thread of the plugin's own owns that connection and redraws on a 25 ms poll, since the API
    thread's tick (250 ms) is slower than a dot's dark phase. Hiding the light joins it.
  - The window manager does not stack an override-redirect window, so nothing keeps it above the
    playfield by itself. The plugin selects `SubstructureNotify` on the root, and after a managed
    top-level window is mapped or restacked (at most once every 50 ms) it stacks the light directly
    above the topmost viewable managed window, never simply on top. Override-redirect windows above
    it, such as menus and screen lockers, stay above it. It is stacked before it is first mapped,
    since a window maps at the top.
  - It is unmapped while VPX is not in front: the active window (`_NET_ACTIVE_WINDOW`, else the
    input focus) or its nearest ancestor with a `_NET_WM_PID` must name VPX's process. SDL sets
    that property. The light goes on that window's monitor (RandR 1.5, else the whole screen),
    scaled by `Xft.dpi`.
  - It always has a bounding shape cut to its disc. With no compositor the server shows each
    pixel's colour as it is, so the shape keeps the more opaque pixels and the rim sits over black.
    A compositor can also stop compositing a fullscreen window with the light above it
    (picom's `unredir-if-possible`), which without the shape left a black square. With no
    compositor, `StatusOpacity` darkens the light rather than blending it.
- Wayland: clients cannot position windows or keep them on top, short of the layer-shell
  protocol, which SDL and most compositors used for VPX don't offer. **Unsupported**: the
  notification stays the surface. The plugin asks SDL for its driver the way VPX does
  (`SDL_GetCurrentVideoDriver`, `Window.cpp:208-220`) and shows the light only under `x11`. If SDL
  can't be found by name, it guesses as SDL 3 chooses: Wayland when `WAYLAND_DISPLAY` is set,
  unless `SDL_VIDEO_DRIVER` says otherwise. The reason is logged once:
  `Status light unavailable, notifications only: ...`.

## What the light shows

One round panel, about 44 points: two dots side by side inside a ring of 30 segments, and a glyph
between the dots. The mapping is `src/StatusLight.cpp`, tested on every platform by
`test/StatusLightTest.cpp`; each window only draws it.

The two dots are the Scorbitron's two status LEDs, in the device's own colours (at full scale; the
device dims its LEDs) and its own blink, wherever the device has the state:

| Dot | State | Colour | Blink |
|---|---|---|---|
| System (left) | ok | green 0,255,0 | 1200 ms on, 100 ms off |
| | offline | orange 255,165,0 | |
| | not paired | red 255,0,0 | |
| | degraded | purple 255,0,255 | |
| Session (right) | no game | blue 0,0,255 | 1189 ms on, 123 ms off |
| | tracking | green 0,255,0 | |
| | no data | amber 255,191,0 | |
| | not authenticated | red 255,0,0, ahead of the game state (protocol 1.2 daemons) | |

The device has no "not paired" or "degraded" light of its own; those two reuse its red and purple.

The ring stands for the tap pad, in a Scorbit style of its own rather than the pad's animations: a
pulsing rainbow while all is well and no game is scored, dark while a game is scored, solid red when
the daemon is not paired or not authenticated, and solid blue with no daemon. It flashes white once
when a player claims a slot.

The glyph is `!` for any state that is expected to work and does not, `?` for a value this build
does not know, and nothing when all is well, so no state is told by colour alone.

With no daemon, after the notifications' 5 s grace, both dots are dark (nothing drives them) and the
ring is blue with `!`. A session still waiting for its first Status, or a loss inside the grace,
keeps the light as it was. When the daemon hides its indicator, there is no light at all.

## Risks, and how the prototype measures them

| Risk | Platform | Measurement |
|---|---|---|
| The window takes focus and pauses the table | all | 5-minute session, window visible and the state changing; `vpinball.log` has no `Playfield window lost focus` |
| Not visible over VPX's fullscreen | macOS | windowed, borderless fullscreen and exclusive fullscreen (`Window.cpp:89-121`, 257-262), each with a cropped screenshot of the corner |
| Hidden by exclusive fullscreen | Windows | VPX's BGFX builds have no exclusive mode; only the DirectX 9 build does (`PlayfieldFullScreen = 2`). Measured: the light shows unless fullscreen optimizations are disabled for VPX; under that legacy exclusive mode it is hidden and the notifications remain. See "Results, Windows hardware" |
| Adds latency in borderless fullscreen | Windows | a window overlapping a fullscreen swap chain can take it off independent flip, adding a frame of latency. Compare `PresentMon` with the window shown and hidden. Measured: see "Results, Windows hardware" |
| Covers game content | all | windowed and pillarboxed tables: the window sits outside the playfield; full-bleed tables: no window by default; with `StatusCorner` set, screenshots of that corner on a few tables |
| Stays on top of other apps | all | switch away from VPX mid-game and back; the window hides and returns |
| Hidden under the playfield | X11 | windowed and fullscreen, with and without a compositor; the window stays above after the playfield is raised |
| Multi-screen cabinets | all | the window follows the playfield's display, not the backglass's; check on a two-display setup |
| Cost on the render path | all | none by design: frame times with and without the window, from VPX's own performance overlay |

## Prototype results, macOS (SB-6106, 2026-10-30)

VPX rev 6461 (`9a8f24cb6`) on Apple silicon, Theatre of Magic, a 5-minute session per mode with
`StatusCorner = 2` and a fake status cycling every 4 s (steady green, then amber, red and blue
pulsing), at `NSStatusWindowLevel`.

| Mode | Focus | Visible |
|---|---|---|
| Windowed (`PlayfieldFullScreen = 0`, 1280x720) | no `Playfield window lost focus` | yes, top right, above the menu bar |
| Fullscreen (`PlayfieldFullScreen = 1`, VPX's own Space) | none caused by the window (one 0.1 s loss traced to the operator) | yes, over the playfield, at 30 s and at the end |

Three findings changed the design above:

- **`hidesOnDeactivate` loses the window.** The game starts (`New game`) about 40 ms before VPX
  becomes the active application. A panel first ordered front while its application is inactive
  is never brought back by AppKit on activation, so nothing showed. Following the activation
  notifications fixes it.
- **A window class defined in the plugin crashes VPX at quit.** VPX unloads the plugin and then
  hides its own windows; AppKit's window cache reached the panel, whose class was in the unloaded
  image (`objc_loadWeakRetained` ← `isWindowObjectInCache`, SIGSEGV).
- **A literal handed to Core Animation crashes VPX at quit.** Removing the pulse queued a change
  keyed by `@"pulse"`, a constant in the plugin's image; the run loop committed it after unload
  (`CFGetTypeID` ← `CA::Render::encode_remove_animation`, SIGSEGV). Heap strings and an explicit
  `[CATransaction flush]` fix it. VPX quits cleanly with both fixes.

Not yet measured, left to the macOS implementation: exclusive fullscreen, switching to another
application and back mid-game, a second display, frame times with and without the window, and the
quit path from VPX's own menu rather than an Apple event.

## Results, Windows (2026-10-31)

VPX rev 6461, x64 and x86 builds, in a Parallels Windows 11 VM on Apple silicon, playing Theatre of Magic with
`StatusCorner = 2` and a protocol 1.2 daemon that reports the device unpaired. Seven sessions, windowed and borderless
fullscreen, with the daemon stopped and restarted in the windowed ones.

- **The light follows the state.** It shows the dark disc, red ring, red and blue dots and `!` for an unpaired
  daemon. While the daemon is stopped it shows dark dots in a solid blue ring with `!`, and it returns when the
  daemon does. This was checked by eye on the VM's screen.
- **Placement.** The window is topmost and layered, 44 by 44 logical pixels, inset from the top-right corner, and
  above VPX's playfield in z-order.
- **No focus taken.** In every session, every `Playfield window lost focus` line names another window: the test
  harness's own consoles, or the Windows firewall prompt for the daemon. None names the light.
- **Clean quit.** A close request unloads the plugin, logs `Player closed`, and VPX exits with code 0.
- **One fix came from the run.** The window ignores `WM_CLOSE`. A close sent to every window of the process
  (taskkill, logoff) would otherwise destroy it behind the plugin's back.

Not measured in the VM: exclusive fullscreen (VPX's `PlayfieldFullScreen = 2`) and the independent-flip latency risk.
VPX fell back to a 1024x768 window there. Both were measured on hardware below.

Measuring this in a VM has pitfalls that each look like a defect in the light:
- a console window opened mid-session takes the foreground, which pauses VPX and hides the light;
- the host's VM screen capture and a GDI capture inside the guest both return black for VPX's GPU-presented frames;
- a close sent from a remote shell never reaches the interactive session's windows;
- the VM pauses itself when idle.

## Results, Windows hardware (2026-11-01)

A physical Windows 11 (26100) laptop. Its 3840x2160 60 Hz panel is driven by the Intel UHD 630, at 250% display
scaling; its NVIDIA GTX 1650 renders but does not drive the panel (`nvidia-smi` reports no active display). VPX rev 6461
(`9a8f24cb6`), x64. Theatre of Magic, `StatusCorner = 2` (light shown) or `0` (hidden), and a protocol 1.2 daemon
reporting the device unpaired. In each session PresentMon 2.6 recorded 45 s of presents while VPX played, and the
light and VPX's notifications were checked by eye on the panel. Screen captures return black, because VPX presents on
a hardware plane.

**Exclusive fullscreen exists only in VPX's DirectX 9 build.**
- In the BGFX builds (`VPinballX_BGFX64.exe`), `PlayfieldFullScreen` offers only Windowed and Borderless Fullscreen
  (`Settings_properties.inl`, under `ENABLE_BGFX`). `RenderDevice.cpp` notes that BGFX does not implement exclusive
  fullscreen, so that backend does not support it.
- The DirectX 9 build (`VPinballX64.exe`) creates its device with `Windowed = false` for `PlayfieldFullScreen = 2`.
  So BGFX D3D11/12 has no exclusive swap chain to hide the light, and the DirectX 9 build is the one measured for
  exclusive fullscreen.

| Build and mode | Light | Present mode | Frame interval, median | Display latency, median (p99) | Light seen |
|---|---|---|---|---|---|
| BGFX, borderless | shown | Hardware Composed: Independent Flip | 16.68 ms | 14.99 ms (31.59) | yes |
| BGFX, borderless | hidden | Hardware Composed: Independent Flip | 16.68 ms | 14.81 ms (31.41) | no |
| DirectX 9, exclusive | shown | Hardware Composed: Independent Flip | 16.66 ms | 7.60 ms (9.93) | yes |
| DirectX 9, exclusive | hidden | Hardware Composed: Independent Flip | 16.67 ms | 8.12 ms (9.63) | no |
| DirectX 9, borderless | shown | Composed: Copy with GPU GDI | 16.67 ms | 32.26 ms (32.55) | yes |
| DirectX 9, borderless (exclusive requested, mode not matched) | hidden | Composed: Copy with GPU GDI | 16.66 ms | 32.27 ms (33.03) | no |
| DirectX 9, exclusive, fullscreen optimizations off | shown | **Hardware: Legacy Flip** | 16.67 ms | 8.63 ms (13.57) | **no** |
| DirectX 9, exclusive, fullscreen optimizations off | hidden | Hardware: Legacy Flip | 16.68 ms | 9.15 ms (13.45) | no |
| DirectX 9, exclusive, optimizations off, GPU preference High performance | shown | Hardware: Legacy Flip | 16.67 ms | 8.49 ms (10.70) | **no** |
| DirectX 9, exclusive, optimizations off, GPU preference High performance | hidden | Hardware: Legacy Flip | 16.67 ms | 8.37 ms (10.56) | no |
| BGFX, borderless, GPU preference High performance | shown | Hardware Composed: Independent Flip | 33.33 ms | 30.96 ms (31.44) | yes |
| BGFX, borderless, GPU preference High performance | hidden | Hardware Composed: Independent Flip | 33.33 ms | 31.08 ms (31.62) | no |

The first six rows are a first pass. The last six are a second pass, after an NVIDIA driver update (621.01). In that
pass, the two per-application settings were set for the test executable for one session each and removed after it:
"Disable fullscreen optimizations" (compatibility layer `DISABLEDXMAXIMIZEDWINDOWEDMODE`) and the GPU preference.

- **Legacy exclusive fullscreen hides the light; the notifications stay visible.**
  - With fullscreen optimizations disabled for VPX, its DirectX 9 exclusive mode is a legacy exclusive swap chain
    (`Hardware: Legacy Flip`), and the topmost light is not shown over it.
  - The plugin still creates the light: its log reads `Status light: corner 2`.
  - The plugin's status notifications keep posting (`[!] Scorbit: no game, not paired`, repeated every minute),
    whether the light is shown or not. VPX draws them in its own frame, so they appear on the panel in legacy
    exclusive mode. That is the fallback, and it needs no change.
- **With fullscreen optimizations, Windows' default, DirectX 9 exclusive is not legacy.** The exclusive request
  presents through the flip model on a hardware plane, and the desktop composes the light over it.
- **The light costs no latency.** Shown and hidden are within 0.6 ms of each other in every mode. VPX stays on
  hardware independent flip with the light up, with the light on its own plane. The DirectX 9 build's borderless mode
  is composed by the desktop, at about two frames of latency, with or without the light.
- **Which GPU rendered.** In the second pass, every DirectX 9 session rendered on the GTX 1650, with or without the GPU
  preference (`nvidia-smi pmon` lists `VPinballX64.exe`, about 65% busy). It used both adapters' 3D engines: the
  GTX 1650 renders and the Intel GPU presents to the panel. BGFX with the GPU preference also rendered on the
  GTX 1650 and ran at 30 fps at this resolution, with the light shown or hidden. The first pass recorded no adapter.
- **The exclusive mode must match one the display lists.** VPX takes `PlayfieldFSWidth`, `PlayfieldFSHeight` and
  `PlayfieldRefreshRate`. It logs `Requested fullscreen mode ... is not available. Switching to windowed mode.` and
  runs borderless when no listed mode matches them. Windows reported this panel at 59 Hz while VPX listed it at
  60 Hz. A refresh rate of 0 matches any. The VM's earlier attempt failed the same way.
- **No focus taken.** No `Playfield window lost focus` line names the light. Only the harness's console at quit, and
  once a Windows Security prompt at start, took focus.

Not measured: a desktop whose NVIDIA (or other discrete) GPU drives the display itself, rather than rendering for an
integrated GPU that drives the panel.

## Results, X11 (2026-10-31)

No Linux desktop was available, so this is not a VPX run. An SDL2 window stands in for the
playfield, in the same process as the plugin's `StatusWindow_x11.cpp`, so `_NET_WM_PID` and
`SDL_GetCurrentVideoDriver` behave as they do under VPX, and SDL's focus-lost event is the signal
VPX pauses on. Xvfb at 1920x1080, Openbox, on Ubuntu 24.04 (arm64), `StatusCorner = 2`, the
unpaired state.

| Mode | Light on top after the playfield and another app are raised | Focus lost | Click on the light |
|---|---|---|---|
| windowed, no compositor | yes | only when switched away | goes to the desktop beneath |
| fullscreen, no compositor | yes | only when switched away | reaches the playfield |
| windowed, picom | yes | only when switched away | goes to the desktop beneath |
| fullscreen, picom | yes | only when switched away | reaches the playfield |
| fullscreen, picom `--unredir-if-possible` | yes | only when switched away | reaches the playfield |

- **Drawing.** Under picom the disc blends: black at 0.6 over the playfield's green reads
  `#004000`, and at `StatusOpacity = 50` it reads `#007000`. With no compositor, and with picom
  unredirecting the fullscreen playfield, the light is cut to its disc with the playfield around
  it. Before the shape was made unconditional, the second case showed a black square.
- **Other apps.** With another application active, the light is unmapped; it is mapped again when
  the playfield is active.
- **Not x11.** Under SDL's `offscreen` driver, a Wayland session, and no display, the light is
  refused with the reason, and the notifications are unaffected.
- **Five minutes.** Fullscreen under picom `--unredir-if-possible`, the playfield raised every
  10 s: the light was on top after all 30 raises, and focus was never lost.
- **Hiding** joins the light's thread in under 25 ms.

Not measured: VPX itself on a Linux desktop, a GNOME or KDE compositor, a second display, and
XWayland.

## Order

1. This note.
2. macOS prototype in a live VPX: the window as above driven by a fake status, then the focus and
   fullscreen rows of the risk table, each a live VPX session with the screen unlocked.
3. An implementation per platform: macOS, Windows, then X11 (all three done).
4. The status indication is already specified as plugin-owned rather than an upstream VPX event.
