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

The plugin has the signal, but not from one place.

- **An authored table** tells the plugin itself: its script calls `game_start` and `game_end`.
  **A game is in progress from a committed `game_start` until a committed `game_end`, or until the
  table ends.** The daemon's session state plays no part.
- **A ROM table** tells the daemon, which reports it in Status as a session state: `no game`,
  `tracking` or `no data` (`src/WireProtocol.h`). **A game is in progress from a Status that says
  `tracking` until one that says `no game`, or until the table ends.** `no data` continues a game
  already in progress and never starts one.

`no data` cannot start a game because the daemon also uses it outside games: briefly at table load
("waiting for this table's first scores", "switching to this table's machine"), and after an
authored game it could not record ("this game was not recorded", held until the next
`game_start`). Counted as a game, those would hide the very notices meant for between games.

A ROM game the daemon cannot track at all (no memory map, not paired) never reaches `tracking`, so
it counts as outside a game: its state posts once when it changes, and repeats every 60 s while it
holds. Those states are known at table load, before the player starts.

**A lost connection.** An authored game is unaffected: the script still decides, and the plugin
replays the game from its `game_start` after a reconnect. A ROM game's end can only come from the
daemon, so a lost connection keeps the last state, and a daemon restart mid-game shows no
"no connection" notice. That hold is bounded: **after 2 minutes with no connection, a ROM game
counts as over** and the no-connection state is posted, so a daemon gone for good is told within
that time, not only when the table ends. An authored game lost that way still ends at its own
`game_end`, and the end-of-game notice carries the no-connection state then.

**A reconnect, on a ROM table.** A restarted daemon says `no game` until it has read the machine
again. So a `no game` within 10 s of a reconnect is deferred, not taken at once: a `tracking` within
those 10 s continues the game, and otherwise the game ends when they are up. A game that really
ended during the outage therefore ends 10 s late, not never.

**Every Status counts.** The worker sees each Status; the notifier sees only the latest record left
in the latch when a frame renders, and two Statuses can arrive between frames (`tracking` then
`no game`). So the ROM game is followed in the worker, which applies the rules above to every
Status and puts the result in the status record: whether a ROM game is in progress, a count of ROM
games started (so a game that began and ended between two frames is still seen), and the last
`[!]` text seen during the current or last game.

## Design

1. **Outside a game, no change.** The current state is posted at table load and on each change.
2. **In a game, no notices.** A change made during a game is not posted then; the light shows it
   if it is up. A notice already on screen when the game starts is taken down. VPX gives each notice
   a fixed lifetime (5 s here), and updating the plugin's own notice with a length of 0 expires it at
   once (`NotificationOverlay::PushNotification` reuses the handle and resets its disappear time).
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

**The caption needs a moving cursor.** A position alone is not enough: VPX hides the cursor on the
next button press without moving it, so a cursor left resting on the light would keep the caption
up over play. The caption shows only when the cursor has moved within the last 3 s and is over the
light. It goes after 3 s without movement, and comes back only on fresh movement. Where the platform
reports cursor visibility cheaply (`GetCursorInfo` on Windows), a hidden cursor also takes it down.

**Hover is a desktop gesture, not a channel.** VPX hides the cursor when play starts. It shows it
again when the mouse moves over the playfield, and hides it on the next button press
(`Player::Player`, the `SDL_EVENT_MOUSE_MOTION` case in `player.cpp`, `InputManager.cpp`). VPX does
not capture the mouse, so the global position is real. A desktop player can move the mouse onto the
light to ask what it means. A cabinet has no mouse, and nobody hovers while playing. So hover
explains the light; it never carries anything a player must see.

Rejected:

- **A modifier key to reveal the text.** Modifier state can be polled on the same tick, with no
  hook (`GetAsyncKeyState`, `CGEventSourceFlagsState`, the mask from `xcb_query_pointer`). But VPX's
  default bindings already use the modifiers as game controls: Shift for the flippers, Ctrl for
  magna-save and the menu, Left Alt for the lockbar (`InputManager.cpp`). A modifier reveal would
  fire during play.
- **A caption shown on each change.** It would be wider than the corner the user cleared for the
  light, so it would cover game content the light does not.

## Work

1. The notice rules and `StatusNotices`, on every platform.
   - The rules live in `StatusNotifier`, so `test/StatusNoticeTest.cpp` covers them with no VPX: no
     notices in a game, taking down a notice on screen at game start, the end-of-game notice
     (current and earlier), the repeat outside a game only, and each setting value.
   - `StatusNotifier::Frame` returns an action, not a string: post this text, expire the notice on
     screen, or nothing. Today an empty string means both "suppressed" and "nothing to do", and
     `VpxSessionSource::PostStatusNotice` returns early on it. The session source expires its
     notice handle with a length of 0. The live check starts a game within 5 s of a table-load
     notice and sees it go.
   - The notifier learns an authored game's boundaries from the script's `game_start` and
     `game_end` as `VpxSessionSource` commits them, and the table kind from its Declare.
   - The ROM game is followed in `SocketWorker`, on every Status: the 2-minute bound on a lost
     connection, a `no game` within 10 s of a reconnect deferred and then taken, and the three
     fields added to the status record. `test/SocketWorkerTest.cpp` covers each, including
     `tracking` and `no game` sent back to back with no frame between.
   - `VpxSessionSource` must tell the notifier when the table ends. Today `OnGameEnd` does not, and
     the notifier is called only from the per-frame callback, which stops with the table. The table
     end clears the game state without posting, since nothing renders after it, so the next table
     starts outside a game. The notifier's table-end call is unit tested with the rest; the wiring
     itself needs VPX, so the live check ends a table mid-game and loads another.
2. The hover caption.
   - First, shared by all platforms: the caption text reaches the window. `UpdateStatusWindow` takes
     only a `LightView`, the rendered light state, and only `ApplyStatusWindow` has the
     `StatusRecord`. The call gains the caption text, built by a pure function from the record
     (`StatusNotifier::Describe` of the current state, or the no-connection line), which
     `test/StatusLightTest.cpp` covers alongside the light's mapping.
   - Then one change per platform: macOS, Windows, X11. Each needs a live check that cursor polling
     finds the light while VPX plays, that the caption shows the current support text, and that it
     takes no focus, using the same five-minute session as `docs/status-window.md`. The check
     includes resting the cursor on the light and pressing a flipper: the caption goes within 3 s.
