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
  changes outside a game, the end-of-game line, the PinMAME hint. With the default
  `StatusNotices = 1` nothing shows during a game; with `2`, alerts show during play as they do now.
  `ShowStatus` and `StatusNotices` keep their meanings.
- **How** it is drawn changes. Where the banner can show, the alert shows there **instead of**
  VPX's notice, never as well as it. Where it can't, VPX's notice is used as today.
- **Only problems.** The banner shows `[!]` and `[?]` states only (decided live, 2026-11-23). A state
  that is fine, such as "no game, ok" at table load or a recovery, is not shown, and it takes
  down a problem banner still up. VPX's notice, where it is the surface, keeps showing every state.
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
  empty box. The banner follows the rules' actions: a post shows it, and an `Expire` hides it at
  once. The rules send `Expire` when a game starts under `StatusNotices = 1`, or when the PinMAME
  hint stops holding. The banner adds no take-down of its own, so `StatusNotices = 2` keeps alerts
  up during play.
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
- **Size.** Sizes are in device-independent units (DIPs: macOS points, or pixels at 96 DPI).
  - The text's character height is a fixed 20 DIPs, 1.5 times VPX's own 13. The first build sized
    it to 1/30 of the display's height; on a 1117-point Mac screen that gave 37 points, which read
    as far too big in the live check.
  - At most 60% of the playfield window's width, word-wrapped, three lines at most; a longer text
    is cut with an ellipsis.
  - Padding is half a line. The icon is one line high.
- **Placement:** centred horizontally in the playfield window, its top a quarter of the way down,
  where VPX's notice is now. It does not depend on `StatusCorner`. Sizing and placing it within
  the window, not the display, keeps it inside a windowed table. The window is found as the light
  finds its screen: VPX's key window on macOS, VPX's foreground window on Windows, the active window
  on X11. In fullscreen the window is the display.
- **Scaling:** from DIPs to device pixels, once, by the light's per-platform rules.
  - macOS: DIPs are points, and the layers render at the screen's backing scale.
  - Windows: device pixels = DIPs × the monitor's DPI (`GetDpiForWindow`) / 96. The font is
    created with a negative `lfHeight` of that many pixels, so the size is a character height.
    This is not the caption's points-to-pixels `MulDiv(pt, dpi, 72)`, which would make it a third
    larger.
  - X11: device pixels = DIPs × `Xft.dpi` / 96.

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
