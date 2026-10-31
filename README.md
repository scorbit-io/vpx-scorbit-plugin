# Scorbit plugin for Visual Pinball X

A [Visual Pinball X](https://github.com/vpinball/vpinball) plugin that connects
an emulated pinball machine to [Scorbit](https://scorbit.io). It is the
open-source half of the integration: it serves the Scorbit daemon, running on
the same computer, the machine's memory and DMD frames over a local socket, and
shows the daemon's overlays and status inside VPX. The daemon is the Scorbit
device: it identifies the game, extracts scores and talks to Scorbit. The plugin
holds no Scorbit account, key or credential, and makes no network connection.

The plugin is a standalone project. Build it, copy the resulting folder into
your VPX plugins directory, enable it in `VPinballX.ini`, and play.

**Status: pre-release.** The plugin and its protocol are complete for emulated
(ROM) tables. Without a Scorbit daemon running, it waits and says so.

## Requirements

- A Visual Pinball X build at or after the commit pinned in
  `cmake/vpx_headers.cmake` (currently master of 2026-10-27, rev 6461), with the
  PinMAME plugin. Memory writes need a PinMAME carrying `WriteMemory:1`
  (vpinball/pinmame #634), which rev 6461 does. The plugin API changes layout between builds and has no
  version handshake, so a plugin loaded into an older VPX does not fail
  cleanly: it corrupts memory during table start. Match the pin to the VPX you
  run, and take VPX from the upstream CI artifacts until a release catches up.
- CMake 3.28 or newer and a C++20 compiler. There are no other dependencies:
  the VPX and PinMAME plugin headers are fetched at configure time, pinned by
  commit and sha256.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows, the Visual Studio generator finds MSVC itself, so no developer
shell is needed:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

For a 32-bit VPX, configure with `-A Win32` instead. That builds
`plugin-scorbit.dll`, which a 32-bit VPX loads through `plugin.cfg`'s
`windows.x86` key, beside the x64 `plugin-scorbit64.dll`. It still talks to the
64-bit Scorbit daemon, so 32-bit VPX is supported on 64-bit Windows.

CI uploads the staged Windows plugins, x64 and x86, as workflow artifacts.

To build and test only the transport — the wire protocol, socket worker and wire
dump — with no VPX headers and no network fetch:

```sh
cmake -B build -DSCORBIT_TRANSPORT_ONLY=ON
cmake --build build -j && (cd build && ctest --output-on-failure)
```

That is what CI builds on every platform in `plugin.cfg`.

The build stages a complete plugin folder at `build/stage/scorbit/`: the
plugin library and `plugin.cfg`.

## Install

Copy `build/stage/scorbit/` into the VPX plugins directory:

| Platform | Plugins directory |
|---|---|
| macOS | `VPinballX_BGFX.app/Contents/PlugIns/` |
| Linux | `<vpx install>/plugins/` |
| Windows | `<vpx install>\plugins\` |

Then enable it in `VPinballX.ini`. VPX creates the section on first launch.
Nothing else is required; the other keys are optional:

```ini
[Plugin.Scorbit]
Enable = 1
```

| Key | Meaning |
|---|---|
| `ShowStatus` | Show Scorbit status notifications during play, default on. Off posts none, with or without a daemon. |
| `socketPath` | Optional. Local socket the Scorbit daemon listens on. Empty uses the per-user default (see below). |
| `dmdDumpFile` | Optional. Write every captured DMD frame to this file (see below). Relative paths resolve against the pref directory. |
| `overlayDropDir` | Optional, demo only. Watch this directory for raw overlay payloads and display them (see below). |
| `StatusCorner` | macOS and Windows. Show the status light in this screen corner: 0 off (default), 1 top left, 2 top right, 3 bottom left, 4 bottom right. Off by default because no corner of a full-bleed table is guaranteed free; see `docs/status-window.md`. `ShowStatus = 0` hides it too. |
| `StatusOpacity` | How strongly the status light draws, 10 to 100 percent, default 100. |

The plugin tells the daemon which ROM PinMAME is running; the daemon identifies
the machine from it and from the ROM itself, and decides which Scorbit machine
the session belongs to.

## DMD frame tap

`src/DmdTap.*` captures the raw DMD frames the emulating controller publishes
through the VPX display API. It reads the controller's own *identify* frames
(discrete shade indices, one byte per pixel) rather than any colorized or
upscaled rendering, polls for new frames on a worker thread, and double-buffers
so a consumer never sees a torn frame.

With `dmdDumpFile` set, frames are written as text: one line per display row,
one hexadecimal digit per pixel (`0` off, up to `3` or `f` depending on the
machine's shade depth), and a blank line after each frame. Verified on WPC (128x32, 4 shades),
SAM (128x32, 16 shades) and Sega 192x64 displays.

## Scorbit daemon transport

`src/SocketWorker.*` connects the plugin to a running Scorbit daemon over a
user-scoped local socket and serves it DMD frames. The daemon is the
rendezvous: it listens, the plugin connects, and a plugin that starts first
simply retries once a second until a daemon appears. Nothing blocks the render
path, and the worker never touches the VPX plugin bus.

| Platform | Socket | Token |
|---|---|---|
| macOS | `$(confstr _CS_DARWIN_USER_TEMP_DIR)/scorbit/vpx.sock` | `vpx.token` beside it |
| Linux | `$XDG_RUNTIME_DIR/scorbit/vpx.sock`, else `/run/user/<uid>/scorbit/vpx.sock` | `vpx.token` beside it |
| Windows | `%LOCALAPPDATA%\Scorbit\vpx.sock` | `%LOCALAPPDATA%\Scorbit\vpx.token` |

`socketPath` in `VPinballX.ini` overrides the socket; the token is then read
from the same directory. The daemon writes the token file, and the plugin reads
it immediately before every connection attempt, so a daemon that restarts and
rotates its token is picked up without restarting VPX.

`src/WireProtocol.*` is the whole wire format: little-endian, a `u32` length
followed by `u16` type, `u16` flags and a `u32` sequence number, then the
payload. This build speaks protocol 1.1 and serves Hello, Declare, Frame, Ping,
Bye, the memory messages, Overlay and Status, and sends Game for table scripts. It
still talks to a 1.0 daemon, without the table-script calls. A type it does not serve is
answered with `unsupported_type` rather than ignored. Frames cross as one shade
index per pixel, row major, exactly as the tap captured them.

`test/` holds a scripted daemon-side peer and the test binaries; `ctest` in the
build directory runs them all. They need neither Visual Pinball nor a daemon.

## DMD overlay

`src/DmdOverlay.*` draws bitmaps over the live DMD the way a Scorbitron does:
upload a bitmap, show it for a duration or until hidden, hide it. VPX has no
draw callback for this, so the overlay is delivered as display-source
overrides, which every consumer of the DMD picks up as original data: VPX's own
DMD, the PUP and B2S DMD overlays, ScoreView, and real DMDs through DMDUtil.
The plugin lists its overrides ahead of every other source, overrides the
controller's display on an uncoloured table and every colouriser output (Serum,
VNI) on a coloured one, and serves each with the cached bitmap composited over
it. The frame tap keeps reading the controller's own frames, and the render path
never waits on whoever writes the overlay. `docs/slice4-internals.md` has the
rules and the thread handoffs.

Placement and shading mirror the probe firmware: the bitmap is centred on the
display, and pixels outside it or flagged transparent (`0x80`) show the live
frame. The low seven bits of a pixel are the platform's bitplane bits, not a
brightness, so each value is drawn at the shade the probe shows it at on that
platform, taken from PinMAME's `hardwareGen` (`src/OverlayState.h`):

| Platform | Shade of value *v* |
|---|---|
| WPC | popcount(*v* & 7) / 3 |
| Data East, Whitestar, Sega 192x64 | (*v* & 3) / 3 |
| Gottlieb System 3 | popcount(*v* & 0x3F) / 6 |
| Capcom, Alvin G | popcount(*v* & 0xF) / 4 |
| SAM | PinMAME's own luminance for framebuffer value *v* & 0xF |

Overlays are normally full frames at the display's own size (128x32, 128x16 or
192x64), shaped by transparency, which places them at the origin. A mismatched
size is handled as the firmware does: a smaller bitmap is centred, a larger one
is centred with its edges cropped, and an upload outside 10x10 to 192x64 is
ignored, leaving the previous overlay in place (`src/OverlayPlacement.h`).

The daemon drives the overlay over the socket with the probe's own two payloads,
and a session ending hides it and drops the bitmap. In process it is driven by
a message on the VPX plugin bus, `Scorbit` / `Overlay`, declared in
`src/ScorbitPluginAPI.h`. Ops are upload (the raw `WriteDmdOverlay` payload:
width, height, then one byte per pixel), show with a duration in milliseconds
(0 for until hidden) and hide. Show arms the overlay even before an upload, as
the probe does. Send it on the API thread.

For testing without a daemon connection, `overlayDropDir` names a directory the
plugin watches. Writing `overlay.bin` (the upload payload) uploads; writing
`overlay-ctl.bin` (three bytes: control, duration high, duration low) shows or
hides by bit 0 of the control byte. Both go through the same message.

## Status

The daemon sends its session and system state, and the plugin logs every one
and shows a VPX notification when it changes, repeated every 60 seconds while a
warning (`[!]`) lasts. Without a daemon session for 5
seconds, the plugin says so itself, with the daemon's reason when it refused the
connection. `ShowStatus = 0` turns all of it off.

## Table scripts

An original table, one with no ROM for the daemon to recognise, can identify itself to
Scorbit and report its game from its script. The calls mirror the Scorbit SDK's:

```vbscript
Dim Scorbit
On Error Resume Next
Set Scorbit = CreateObject("Scorbit.Table")
On Error Goto 0
If IsObject(Scorbit) Then Scorbit.Declare "<machine uuid>"

' later, from the table's game logic
If IsObject(Scorbit) Then
   Scorbit.SetGameStarted 0          ' 0 start button, 1 lobby
   Scorbit.SetCurrentBall 1
   Scorbit.SetActivePlayer 1
   Scorbit.SetScore 1, 12340         ' player, score[, feature]
   Scorbit.AddMode "MB:Multiball"
   Scorbit.Commit
End If
```

VPX hands that `CreateObject` to this plugin. Without the plugin it fails, which is why
the script guards every use: the table must still run on a VPX without Scorbit.

| Call | Does |
|---|---|
| `Declare uuid` | names the Scorbit machine: a uuid in 8-4-4-4-12 hex, any case |
| `SetGameStarted origin` | 0 start button, 1 lobby |
| `SetGameFinished` | ends the game and commits |
| `SetCurrentBall ball`, `SetActivePlayer player` | |
| `SetScore player, score[, feature]` | score up to 2^53; feature defaults to 0 |
| `AddMode mode`, `AddModeExpiring seconds, mode` | mode names up to 128 bytes |
| `SetModeCompleted mode`, `RemoveMode mode`, `ClearModes` | |
| `Commit` | sends every call since the last commit, in order |

Calls are held in the plugin until `Commit`. An argument that does not fit, such as a
malformed uuid, a score past 2^53 or a long mode name, is logged and ignored rather than
raised as a script error; a call with the wrong number of arguments is still a script error.
Game calls made before `Declare`, or before the table starts, are dropped. The script runs
before the table starts, so a `Declare` made then goes to the daemon at table start; one
made later goes at once.

When PinMAME runs a ROM the ROM wins: `Declare` and the game calls are logged and ignored.
A daemon that does not serve table scripts (protocol 1.0, or without the capability) gets
the table as it would without a `Declare`, and no game calls.

With a `Declare`, the plugin sends the daemon the table, VPX and preferences folders, which
it needs to find the table's script. Those paths never appear in the log, and a wire dump
blanks them.

### Reading back

`Scorbit.Table` also exposes read-only state a script can poll from a timer. Nothing here
raises a script error: an index out of range, or a call while no daemon can answer, reads as
`""`, `0`, `False` or the "none" value given below.

| Member | Type | Reads |
|---|---|---|
| `Version` | String | the plugin's version |
| `ApiLevel` | Integer | 2: the game calls and everything in this table. An object without `ApiLevel` is level 1, the game calls only |
| `SystemState` | Integer | 0 ok, 1 not paired, 2 offline, 3 degraded; -1 before the daemon's first status |
| `SessionState` | Integer | 0 no game, 1 tracking, 2 no data, 4 indicator hidden; -1 before the first status |
| `AuthStatus` | Integer | 0 not authenticated, 1 authenticating, 2 checking pairing, 3 unpaired, 4 paired, 5 authentication failed; -1 until the daemon reports it |
| `StatusText` | String | the daemon's support text, often empty |
| `MachineUuid` | String | the uuid the table declared; `""` before `Declare` and while a ROM runs, since the ROM then identifies the table and the uuid is not sent |

**Leaderboards.** A request returns an id, and the result arrives later under that id:

```vbscript
id = Scorbit.RequestTopScores(0, 0, 0, 10, "")   ' scope, period, vpinFilter, limit, since
' later, from a timer
If Scorbit.TopScoresResult(id) = 0 Then
   For i = 0 To Scorbit.TopScoresCount(id) - 1
      Debug.Print Scorbit.TopScoresRank(id, i) & " " & Scorbit.TopScoresName(id, i) & " " & Scorbit.TopScoresScore(id, i)
   Next
   Scorbit.ReleaseTopScores id
End If
```

| Member | Type | Reads |
|---|---|---|
| `RequestTopScores(scope, period, vpinFilter, limit, since)` | Integer | a request id, or 0 when no daemon can serve leaderboards now (none connected, an older one, or the table not yet started) or an argument is out of range. scope 0 machine, 1 variant, 2 game; period 0 all time, 1 14 days, 2 30 days, 3 90 days, 4 180 days, 5 365 days; vpinFilter 0 any, 1 virtual only, 2 physical only; limit 1 to 20; since `""` or a UTC ISO-8601 time (up to 32 bytes) that replaces period |
| `TopScoresResult(id)` | Integer | -1 no result yet, or an id not held; 0 ok, 1 not paired, 2 Scorbit error, 3 failed, 4 superseded by a table change; 5 no table declared, 6 not supported by the daemon, 7 the request or its answer was malformed, 8 connection lost before the answer |
| `TopScoresAge(id)` | Integer | seconds since Scorbit was asked for these scores |
| `TopScoresCount(id)` | Integer | entries, 0 unless the result is 0 |
| `TopScoresRank(id, i)`, `TopScoresScore(id, i)` | Integer, Double | entry `i`, from 0. Scores are exact up to 2^53 |
| `TopScoresName(id, i)`, `TopScoresInitials(id, i)`, `TopScoresCreated(id, i)` | String | display name, initials, and when the score was made, as Scorbit gives it |
| `TopScoresFlags(id, i)` | Integer | bit 0 played on a virtual table, bit 1 verified, bit 2 verified by NFC |
| `ReleaseTopScores(id)` | | forgets a result |

The plugin keeps the eight most recent results; a ninth evicts the oldest, whose id then
reads as -1. The daemon answers repeated requests from a cache, so polling the result is
cheap, and a request repeated within a minute may be answered with the scores it already holds.

**Players.** Slots are 1 to 9, as the game numbers its players.

| Member | Type | Reads |
|---|---|---|
| `PlayerClaimed(slot)` | Boolean | a Scorbit player has claimed the slot |
| `PlayerId(slot)`, `PlayerName(slot)`, `PlayerInitials(slot)` | String | the claimed player; the name is their preferred name when they set one |
| `PlayerClaimLink(slot)` | String | for an unclaimed slot, the link a player opens to claim it, for a QR code on the table |
| `PlayerPicture(slot)` | String | the full path of the claimed player's picture, a JPEG, or `""` |

A picture file belongs to the plugin and lasts until the slot's claim or picture changes,
the table ends, or the plugin unloads; load it, or copy it, rather than keeping the path.

Leaderboard results, players and pictures belong to the table: they are cleared when the
table ends, and players and pictures also when the connection to the daemon ends. A daemon
that does not serve them leaves every member at its "none" value, so a script written for
them still runs.

### Gameplay settings

Scores are only comparable between games played under the same rules. Expose every setting
that changes how a score is made as a VPX Table Option: balls per game, extra balls,
difficulty, tilt sensitivity, rule toggles. Players then see them in VPX's options menu, and
the table's settings file records them.

```vbscript
Sub Table1_OptionEvent(ByVal eventId)   ' 0 table started, 1 option changed, 3 menu closed
   OptionBallsPerGame = Table1.Option("Balls per game", 3, 5, 1, 3, 0, Array("3", "4", "5"))
End Sub
```

- `Table.Option(name, min, max, step, default, unit[, labels])` registers the option the first
  time it is read and returns its value. `unit` is 0 for a plain number, or 1 to show a fraction
  from 0 to 1 as a percentage. With `labels`, `step` must be 1, `min` and `max` whole numbers, and
  there must be one label per value.
- Read options in `<TableName>_OptionEvent`, not in `_Init`: VPX raises the option event after
  `_Init`, and again whenever the player changes an option.
- Apply a changed setting at the next game start, never during a game.
- Keep option names stable. VPX stores each value under its option's name, so renaming an option
  loses what players have set. Names that differ only in spaces or punctuation are the same
  option.

This is best effort. An option left at its default may have no entry in the settings file, and
VPX can be pointed at another settings file, so the file alone does not always give the values a
game was played under.

[`docs/sample-table.vbs`](docs/sample-table.vbs) puts it all together: a guarded
`Scorbit.Table`, `Declare`, the game calls, a balls-per-game option, and the reading-back
members polled from a timer.

## Layout

```
src/            plugin sources (ScorbitPluginAPI.h is the message contract)
test/           transport tests and the scripted daemon-side peer
plugin.cfg      VPX plugin manifest (ids and library names per platform)
cmake/          the pinned VPX/PinMAME header fetch
```

Run the tests with:

```sh
cmake --build build -j && (cd build && ctest --output-on-failure)
```

## Credits

The original prototype was written by [jsm174](https://github.com/jsm174)
as a branch of Visual Pinball X.

## AI assistance

Parts of this project were developed with AI assistance. All code is reviewed,
built and tested by Scorbit engineers before it is committed.

## License

GPL-3.0-or-later, see `LICENSE`. The VPX plugin API headers this project
compiles against are GPLv3+ as well.
