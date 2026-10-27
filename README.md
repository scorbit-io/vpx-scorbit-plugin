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
  `cmake/vpx_headers.cmake` (currently master of 2026-10-17, rev 6289), with the
  PinMAME plugin. Memory writes need a PinMAME carrying `WriteMemory:1`
  (vpinball/pinmame #634), which rev 6289 does. The plugin API changes layout between builds and has no
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

CI uploads the staged Windows plugin as a workflow artifact.

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
machine's shade depth), and a blank line after each frame. That is the format
Scorbit's `vpin2bin` tool consumes, so a dump from VPX can be compared directly
against captures from physical machines. Verified on WPC (128x32, 4 shades),
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
payload. This build speaks protocol 1.0 and serves Hello, Declare, Frame, Ping,
Bye, the memory messages, Overlay and Status. A type it does not serve is
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
and shows a VPX notification when it changes. Without a daemon session for 5
seconds, the plugin says so itself, with the daemon's reason when it refused the
connection. `ShowStatus = 0` turns all of it off.

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
