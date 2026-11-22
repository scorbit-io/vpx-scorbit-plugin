# Status alerts as a banner (design, SB-6869)

## Why

The plugin's alerts go through VPX's notification call, which takes text and a duration and
nothing else. VPX draws every plugin's notice the same way: 13 px DroidSans scaled by the
display's DPI, white on a dark box at 67% opacity, centred a quarter of the way down the screen
(`NotificationOverlay.cpp`, `LiveUI.cpp`). Over a lit, flashing playfield that is small and hard
to read, and VPX offers no setting for colour, size, position or opacity.

The plugin already draws its own windows: the status light and its hover caption
(`docs/status-window.md`), on macOS, Windows and Linux X11. This note uses the same kind of window
to draw the alerts.

## What changes, and what does not

- **When** an alert shows is unchanged. `docs/status-notices.md` still decides: table load,
  changes outside a game, the end-of-game line, the PinMAME hint. Nothing shows during a game.
  `ShowStatus` and `StatusNotices` keep their meanings.
- **How** it is drawn changes. Where the banner can show, the alert shows there **instead of**
  VPX's notice, never as well as it. Where it can't, VPX's notice is used as today.
- The status light, when `StatusCorner` is set, is untouched.

## The banner

- **A window of the light's kind, above VPX's window.** It is not drawn into VPX's frame: the plugin
  API has no way into the playfield image except the fixed-style notice.
  - It never takes focus, and clicks pass through it.
  - It shows only while VPX is the active application, as the light does.
  - The macOS, Windows and X11 window rules in `docs/status-window.md` apply unchanged. That
    covers the level and stacking, the active-application checks, `.fullScreenAuxiliary`, and the
    empty input shape on X11.
- **Only while an alert is up.** It appears when VPX's notice would, for the same lifetime
  (`StatusNotifier::NOTICE_LIFETIME`, 5 s), then hides. Nothing is on screen otherwise, not even an
  empty box. A take-down (the game starts, the hint stops holding) hides it at once.
- **Look:**
  - A solid black rounded box with large text in yellow (`#FFD400`).
  - An icon at the start of the first line replaces the `[!]` / `[OK]` / `[?]` marker:
    - problem: a yellow triangle with "!";
    - working: a white circle with "i";
    - unknown: a white circle with "?".
  - The icons are drawn as shapes, not emoji characters. Emoji fonts differ between macOS and
    Windows, and X11 often has none.
  - The icon keeps the shape difference the markers exist for, so no state is told by colour
    alone.
- **Size, relative to the playfield's display:**
  - Text height is 1/30 of the display's height, clamped to 18–48 points: about 36 px on a 1080p
    screen, against VPX's 13.
  - At most 60% of the display's width, word-wrapped, three lines at most; a longer text is cut
    with an ellipsis.
  - Padding is half a line. The icon is one line high.
- **Placement:** centred horizontally, its top a quarter of the way down the playfield's display,
  where VPX's notice is now. It does not depend on `StatusCorner`. The display is found as the
  light finds it: the key window's screen on macOS, VPX's foreground window's monitor on Windows,
  the active window's monitor on X11.
- **Scaling:** the light's per-platform rules apply.
  - macOS: points, rendered at the screen's backing scale.
  - Windows: the monitor's DPI (`GetDpiForWindow`).
  - X11: `Xft.dpi`.

## Text on each platform

- **macOS:** an `NSTextField` in a non-activating panel, as the caption uses, with a bold system
  font. The icon is a `CAShapeLayer`.
- **Windows:** GDI text drawn into the layered window's bitmap, as the caption does (`DrawTextW`,
  ClearType off so each pixel's alpha stays exact). Segoe UI Semibold. The icon is a polygon or
  ellipse drawn into the same bitmap.
- **X11:** the caption uses a core font the server draws, preferring a scalable Helvetica and
  falling back to `fixed`.
  - `fixed` cannot be scaled to the banner's size. So the banner asks for a scalable font at its
    pixel size, and if the server has none, it does not show; the alert goes to VPX's notice.
  - The icon is `xcb_fill_poly` and `xcb_poly_fill_arc`.

## When the banner can't show

The banner shows wherever the status light could. Otherwise the alert goes to VPX's notice, the
plugin logs the reason once, and the text keeps its `[!]` marker.

The banner can't show in these cases:
- **Linux on Wayland:** no client can place a window above another (`docs/status-window.md`).
- **X11 with no scalable font,** as above.
- **A window that could not be created** on any platform.
- **The DirectX 9 build in exclusive fullscreen** (`PlayfieldFullScreen = 2`) with fullscreen
  optimizations off. Windows then uses legacy flip, and no window can cover the game
  (`docs/status-window.md`, "Results, Windows hardware").
  - The plugin can't see that from its window: the window is created and reports itself shown.
  - So the plugin reads `PlayfieldFullScreen` from `VPinballX.ini` under `GetVpxInfo`'s preference
    path, and uses VPX's notice whenever the DirectX 9 build runs with value 2.
  - The BGFX builds have no exclusive mode.

A new setting, `StatusBanner`, chooses the surface: 1 the banner (default), 0 VPX's notice always.
A player who sees no alerts in a mode this note did not foresee can set 0.

## Not changed

- The notice rules and their tests. `StatusNotifier` and `PinMameHint` still return the same
  actions. Only the step that hands an action to VPX now hands it to the banner when the banner
  can show.
- The texts, apart from the marker becoming an icon.
- The hover caption, which stays small beside the light.

## Open

- **Placement:** centred, as above, or anchored to the light's corner when `StatusCorner` is set.
  This note proposes centred, because it does not depend on the light and sits where players
  already look for alerts.
- **A short fade in and out** (150 ms) where the platform animates it for free (Core Animation,
  layered-window alpha), or none, so all three platforms match. This note proposes none.

## Work

1. The banner window and its drawing on macOS, behind `StatusBanner`, checked in a live VPX run.
2. Windows, checked on Windows hardware.
3. X11, with the font check and the fallback.
4. README settings table.
