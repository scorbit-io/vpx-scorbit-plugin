# Status notices during play (design, SB-6341)

Status: accepted, not yet implemented. "Work" at the end lists the changes.

## Why

The plugin shows the daemon's Status in two places: the status light (`docs/status-window.md`), and
VPX notifications, text that VPX draws across the playfield for 5 s. Today the notifications follow
`StatusNotifier::Frame` (`src/StatusNotice.cpp`):

- a notice is posted whenever the text changes, including when only the daemon's support text
  changes;
- a `[!]` state that holds is posted again every 60 s (`REFRESH_EVERY`);
- a new table posts the current state once;
- `ShowStatus = 0` turns off the notices and the light together.

Whether a game is in progress plays no part in any of this. In a game played with the network
blocked, the daemon's support text changed several times ("API unreachable", then "can't reach
Scorbit to switch tables, retrying"). Each change put a notice over the playfield, and the 60 s
repeat added more, while the light already showed the same state by colour. The game itself played
and was kept.

## What a notice is for

A notice is worth covering the playfield only when it tells the player something they can act on,
or something they would want to know before playing:

- **Before a game**, at table load: not paired, not authenticated, "map this table in Scorbit
  Desktop", no daemon. The player can fix these, or choose not to play a game that will not count.
- **During a game** there is nothing to do but play. A game played while Scorbit is unreachable is
  still kept: the daemon holds it, and after a reconnect the plugin replays its start (#45). A notice
  mid-game only distracts.
- **After a game**, a state that went wrong during it is worth one line, so the player knows why a
  score might show up late.

## "In a game"

The plugin already has the signal. The daemon's Status carries a session state: `no game`,
`tracking` or `no data` (`src/WireProtocol.h`). The light uses it to darken its ring while a game is
scored. **A game is in progress from a Status that says `tracking` or `no data` until one that says
`no game`, or until the table ends.**

A connection to the daemon lost mid-game does not end the game at once. The plugin keeps the last
state it had, so a daemon restart mid-game shows no "no connection" notice. The hold is bounded:
**after 2 minutes with no connection, the game counts as over** and the no-connection state is
posted. A daemon that is gone for good is told within that time, not only when the table ends.

## Design

1. **Outside a game, no change.** The current state is posted at table load and on each change.
2. **In a game, no notices.** A change made during a game is not posted then; the light shows it
   if it is up.
3. **At the end of a game, one notice if anything went wrong during it.** The plugin remembers the
   last `[!]` text seen during the game. At game end it posts the current state if that is `[!]`.
   Otherwise, if a `[!]` state came up during the game, it posts that one, marked as earlier
   ("during the game: ..."). The state at game end alone is not enough: the end is a `no game`
   Status, which reads `[OK]` after a game that ran unscored in `no data`.
4. **The 60 s repeat of a `[!]` state stays outside a game, as today, and stops in one.**
5. **None of this depends on the light.** It applies with `StatusCorner = 0` (the default),
   on Wayland, and in legacy exclusive fullscreen alike. That matters because the plugin cannot
   tell when the light is hidden. In Windows' legacy exclusive mode the light is created and logged
   as shown, but it never reaches the screen (`docs/status-window.md`, "Results, Windows hardware").
   A rule of "notices only when the light can't show" would have nothing reliable to test.

Considered and not taken, for players without a light: post a change mid-game only when the
marker or the states change (not the support text alone), only after the new state has held for
10 s, and never repeat it. That is one notice for an outage instead of several, but it still covers
the playfield during play.

### A setting

`StatusNotices`, an integer like `StatusCorner`:

| Value | Notices |
|---|---|
| 0 | never; the light only |
| 1 | outside a game, plus the end-of-game line (default) |
| 2 | always, as today |

`ShowStatus = 0` still turns everything off. There is no "actionable only" value. The plugin sees
the session and system states, the authentication status and a free-text support line. Telling
"map this table" from "retrying" would mean matching the daemon's wording, or adding a field to the
protocol. Those states come up at table load, outside a game, so value 1 shows them anyway.

## Hover

The ticket asks for the support text on the light's rollover. The light is click-through and must
never take focus. That rules out mouse events on the window, but not hover itself.

**Polling the cursor needs no input.** On the tick that already drives the light, the plugin reads
the global cursor position and tests it against the light's frame. On macOS that is
`NSEvent.mouseLocation`, on Windows `GetCursorPos`, and on X11 `xcb_query_pointer` on the light's own
connection. The window stays click-through and non-activating, so focus is never at risk. While the
cursor is over the light, a second window of the same kind shows the support text beside it, on the
side toward the centre of the screen.

**Hover is a desktop gesture, not a channel.** VPX hides the cursor when play starts. It shows it
again when the mouse moves over the playfield, and hides it on the next button press
(`Player::Player`, the `SDL_EVENT_MOUSE_MOTION` case in `player.cpp`, `InputManager.cpp`). VPX does
not capture the mouse, so the global position is real. A desktop player can move the mouse onto the
light to ask what it means. A cabinet has no mouse, and nobody hovers while playing. So hover
explains the light; it never carries anything a player must see.

Rejected:

- **A modifier key to reveal the text.** That needs a global key hook: an event tap (behind the
  Accessibility permission) on macOS, a low-level keyboard hook on Windows, a key grab on X11. Each
  is intrusive for a plugin running inside VPX.
- **A caption shown on each change.** It would be wider than the corner the user cleared for the
  light, so it would cover game content the light does not.

## Work

1. The notice rules and `StatusNotices`, on every platform. All of it is in `StatusNotifier`, so
   `test/StatusNoticeTest.cpp` covers it with no VPX: no notices in a game, the end-of-game notice
   (current and earlier), the 2-minute bound on a lost connection, the repeat outside a game only,
   and each setting value.
2. The hover caption, one change per platform: macOS, Windows, X11. Each needs a live check that
   cursor polling finds the light while VPX plays and that the caption takes no focus, using the same
   five-minute session as `docs/status-window.md`.
