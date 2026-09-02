# Scorbit plugin for Visual Pinball X

A [Visual Pinball X](https://github.com/vpinball/vpinball) plugin that connects
an emulated pinball machine to [Scorbit](https://scorbit.io): score tracking,
session upload and player pairing, driven by the game states and DMD frames
that VPX's PinMAME plugin publishes.

The plugin is a standalone project. Build it, copy the resulting folder into
your VPX plugins directory, enable it in `VPinballX.ini`, and play.

**Status: prototype.** Score tracking works for machines listed in
`assets/scorbit_machines.json`. Pairing is by QR code in the log. Nothing here
is production-ready yet.

## Requirements

- Visual Pinball X 10.8.1 (Beta) with the PinMAME plugin. The plugin API
  headers are fetched at configure time from the pinned VPX and PinMAME commits
  in `cmake/vpx_headers.cmake`; a VPX build older than that pin may refuse to
  load the plugin.
- CMake 3.28 or newer and a C++20 compiler.
- To build the Scorbit SDK from source (the default): OpenSSL and libarchive
  development packages on the host. Everything else is fetched by CPM. The SDK
  commit is pinned by `SCORBIT_SDK_GIT_TAG` in `CMakeLists.txt`.

## Secrets

Two values are needed for a build that can talk to Scorbit, and neither is in
the repository. Put them in an untracked `.env` at the repository root (copy
`.env.example`) or export them in the environment; the environment wins.

| Variable | Purpose |
|---|---|
| `SCORBIT_PROVIDER_KEY` | Encrypted provider key, compiled in as the default `providerKey` setting |
| `SCORBIT_SDK_ENCRYPT_SECRET` | Lets the SDK decrypt provider keys at runtime; only needed when building the SDK from source |

Without `SCORBIT_PROVIDER_KEY` the plugin builds but stays disabled until the
user sets `providerKey` in `VPinballX.ini`. Without `SCORBIT_SDK_ENCRYPT_SECRET`
an SDK built from source cannot authenticate; point the build at a prebuilt SDK
instead. Both are read at configure time, so re-run `cmake -B build` after
changing them.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

macOS with Homebrew needs the dependency roots spelled out:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DLibArchive_ROOT=$(brew --prefix libarchive)
```

To use a prebuilt or installed SDK instead of building it:

```sh
cmake -B build -DSCORBIT_SDK_ROOT=/path/to/scorbit_sdk
```

The build stages a complete plugin folder at `build/stage/scorbit/`: the
plugin library, `plugin.cfg`, the SDK library and `assets/`.

## Install

Copy `build/stage/scorbit/` into the VPX plugins directory:

| Platform | Plugins directory |
|---|---|
| macOS | `VPinballX_BGFX.app/Contents/PlugIns/` |
| Linux | `<vpx install>/plugins/` |
| Windows | `<vpx install>\plugins\` |

Then enable and configure it in `VPinballX.ini`. VPX creates the section on
first launch; the keys are:

```ini
[Plugin.Scorbit]
Enable = 1
provider = vpxplugin
environment = staging
providerKey = <encrypted provider key>
deviceKeyFile = scorbit_device.key
logLevel = 1
dmdDumpFile =
```

| Key | Meaning |
|---|---|
| `provider` | Scorbit provider id |
| `environment` | `staging` or `production` |
| `providerKey` | Encrypted provider key issued by Scorbit; defaults to the one compiled in, if any |
| `deviceKeyFile` | Per-machine device key, relative to the VPX pref directory |
| `logLevel` | 0 quiet, 1 info, 2 debug |
| `dmdDumpFile` | Optional. Write every captured DMD frame to this file (see below). Relative paths resolve against the pref directory. |
| `overlayDropDir` | Optional, demo only. Watch this directory for raw overlay payloads and display them (see below). |

The plugin identifies the running machine by the ROM id PinMAME reports and
looks it up in `assets/scorbit_machines.json`, which maps ROM ids to Scorbit
machine ids. A ROM with no entry leaves the plugin idle.

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

## DMD overlay

`src/DmdOverlay.*` draws bitmaps over the live DMD the way a Scorbitron does:
upload a bitmap, show it for a duration or until hidden, hide it. VPX has no
draw callback for this, so the overlay is a render-frame provider: the plugin
publishes its own display source overriding the controller's, and serves the
controller's frame with the cached bitmap composited over it. Identify frames
pass through untouched, so colorizers and the frame tap still see the real
display, and nothing in the render path waits on anything but a short local
lock.

Placement and shading mirror the probe firmware: the bitmap is centred on the
display, pixels outside it or flagged transparent (`0x80`) show the live frame,
and the brightness bits drive the display's bitplanes directly, so a
2-bitplane display sums bits 0 to 2 and a 4-bitplane display uses the low
four bits.

The overlay is driven by a message on the VPX plugin bus, `Scorbit` /
`Overlay`, declared in `src/ScorbitPluginAPI.h`. Ops are upload (the raw
`WriteDmdOverlay` payload: width, height, then one byte per pixel), show with a
duration in milliseconds (0 for until hidden) and hide. Send it on the API
thread.

For testing without a daemon connection, `overlayDropDir` names a directory the
plugin watches. Writing `overlay.bin` (the upload payload) uploads; writing
`overlay-ctl.bin` (three bytes: on/off, duration high, duration low) shows or
hides. Both go through the same message.

## Layout

```
src/            plugin sources (ScorbitPluginAPI.h is the message contract)
assets/         ROM id to Scorbit machine id mapping
plugin.cfg      VPX plugin manifest (ids and library names per platform)
cmake/          CPM bootstrap and the pinned VPX/PinMAME header fetch
```

## Credits

The original prototype was written by [jsm174](https://github.com/jsm174)
as a branch of Visual Pinball X. The game states the plugin consumes are
published by VPX's PinMAME plugin from Tom Collins'
[pinball memory maps](https://github.com/tomlogic/pinmame-nvram-maps).
QR encoding is [Nayuki's qrcodegen](https://www.nayuki.io/page/qr-code-generator-library).

## AI assistance

Parts of this project were developed with AI assistance. All code is reviewed,
built and tested by Scorbit engineers before it is committed.

## License

GPL-3.0-or-later, see `LICENSE`. The VPX plugin API headers this project
compiles against are GPLv3+ as well.
