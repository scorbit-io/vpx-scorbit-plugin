# The Scorbit SDK for VBScript tables (design)

Status: design. Nothing here is implemented yet.

## What it is

A table script reports its game to Scorbit through the native `Scorbit.Table` object the plugin
provides (README, "Table scripts"). Used directly, that object takes boilerplate on every call:
create it guarded, check `IsObject`, guard each call. The **Scorbit SDK for VBScript** is the layer
table authors use instead: a binding of `scorbit_sdk`, kept and released in the `scorbit_sdk`
repo beside its other language bindings, MIT licensed as the SDK is, and documented there as the
same API with the same names. A table author can do what a machine developer does.

The plugin makes no network connection. Every call goes over a local socket to the Scorbit
daemon, whose embedded SDK talks to Scorbit; read-back data comes back the same way, and a player
picture arrives as a local file.

| Layer | Where | Role |
|---|---|---|
| `scorbit-<version>.vbs`, the binding | `scorbit_sdk` | The API authors use: SDK names, guarded, a no-op without the plugin |
| `Scorbit.Table`, the native object | this plugin | The contract the binding calls, versioned by `ApiLevel` |
| The daemon | Scorbit | Holds the SDK; answers the plugin over the local socket |

## Names

The SDK's own names with a `Scorbit` prefix: `ScorbitSetScore`, `ScorbitAddModeExpiring`,
`ScorbitSetModeCompleted`, `ScorbitGetStatus`, and constants as `SCORBIT_STATUS_…`. VBScript has one
flat global namespace, shared with `core.vbs`, the table's own names and any other plugin's script,
so unprefixed or shortened names would collide. Every routine has its own error guard, so nothing
in the binding can raise into a table.

Arguments follow the SDK's order, and the binding translates where the native object differs:
`ScorbitAddModeExpiring mode, seconds` calls `AddModeExpiring seconds, mode`.

**Every argument is required.** A VBScript `Sub` or `Function` has a fixed number of parameters,
with no optional ones, so the binding cannot reproduce an SDK default by omission. Where the SDK
has a default (the expiry's seconds, `setScore`'s feature, `vpinFilter`), the binding's docs give
the value, and the table passes it, for example `ScorbitSetScore 1, 12340, 0`. The parity test
checks each routine's parameter count against the SDK's full signature.

Scores are VBScript numbers, so they are exact up to 2^53; the native object refuses a larger
magnitude rather than round it, and the binding documents the range.

## Feature parity

The binding covers the SDK's game-facing API:

| SDK (`GameState`, events) | `Scorbit.Table` today | In the binding |
|---|---|---|
| `setGameStarted`, `setGameFinished`, `setCurrentBall`, `setActivePlayer`, `setScore` (with feature), `addMode`, `addModeExpiring`, `removeMode`, `clearModes`, `commit` | yes (`AddModeExpiring` takes its arguments in the reverse order) | yes, in the SDK's argument order (above) |
| Achievements: `setModeCompleted` (completed modes are matched against achievement conditions) | yes | yes |
| The SDK's fuller achievements API, when released (events, player progress, frames, refresh) | no | joins the parity set on release; each call gets the same verdict, implemented, present but disabled, or out of scope |
| `getStatus`, `PairingStatusChanged` | no | yes, as a property carrying the SDK's six authentication states (not authenticated, authenticating, checking pairing, unpaired, paired, authentication failed). The daemon's status adds this field in protocol 1.2, separate from the indicator's system state, and sends it even while the indicator is hidden |
| Game status indication (in session, tracking, the reason when not) | no | yes, from the daemon's status: session state, system state and support text |
| `getMachineUuid` | no | yes: the uuid the table declared |
| `getMachineSerial` | no | out: it is the Scorbit device's serial, which on a VPX install is the daemon's, shared by every table; a table's own identity is the uuid above |
| `requestTopScores(scope, period, since, vpinFilter)` | no | yes, polled. Every argument as the SDK has it: scope machine, variant or game; period all time or 14 to 365 days; `vpinFilter` any (the SDK default, passed explicitly), virtual only or physical only. Replies are cached and capped (top N, names truncated) |
| `PlayersUpdated`, `player_info.h` | no | yes, polled. Per slot: the claimed player's name and id, or, when unclaimed, the claim deeplink, so a table can show a claim QR as a machine does |
| `PlayerPictureReady` | no | yes, polled, as a local file path valid for the session (usable through FlexDMD, for example) |
| `GameStartRequested` (start from the app) | no | **present, commented out** (below) |
| `setCreditsDropped`, `setCreditsStatus`, `PricingReceived`, `CreditsAddRequested`, `CreditsStatusRequested` | no | **present, commented out** (below) |
| Device plumbing (diagnostics, device state, probe firmware, downloads, `requestPairMachine`) | no | out: Scorbitron internals, not a game developer's API |
| `requestPairCode`, `requestUnpair`, `getPairDeeplink` | no | out: Scorbit Desktop and the plugin manage pairing |

VBScript handles asynchronous callbacks poorly, so every answer and event is **polled**, the
binding exposing them as properties and a `ScorbitPollEvent` loop the table calls from a timer.
The player list is a state, so the latest is enough. A leaderboard request is not: each request
returns an id, and its completion (result, data and age) is kept under that id until the script
releases it, the oldest dropped beyond a small bound, so two requests that finish between polls
both arrive and each says which request it answers.

Each new native feature has its own capability bit in the plugin's handshake with the daemon, so
a newer daemon with an older plugin, or the reverse, offers the features both have.

### Credits, pricing and starting from the app

The SDK's credits, pricing and remote-start calls apply to cabinets in public, coin-operated use.
They are in the binding, **commented out**, each with a comment saying what it does and that the
plugin does not serve it. Their names and signatures are reserved now, so they cannot clash with
anything added later.

On an authored table, starting from the app also needs the table's cooperation: its game logic is
in the script, so nothing outside it can press start, and a table that enables the call starts its
own game when it sees the request. (A ROM table is started through the emulated machine and needs
none of this.)

## Delivery and versions

The binding is versioned and never edited after release. Each release is its own file, named for
its exact version, `scorbit-<major>.<minor>.<patch>.vbs` (`scorbit-1.0.0.vbs` first), carrying
`SCORBIT_SDK_VERSION`. A table loads the exact version it was written for:

```vbscript
On Error Resume Next
ExecuteGlobal GetTextFile("scorbit-1.0.0.vbs")
On Error Goto 0
ScorbitInit "<machine uuid>"
```

Tables usually share one folder, and VPX finds a file there before any other copy. With exact
names, two tables that ship the same file ship identical bytes, so neither can replace the copy
the other needs, and tables written against different releases load different files.

`GetTextFile` resolves through VPX's script search (`FileLocator::SearchScript`, vpinball
`9a8f24cb6`, `src/core/FileLocator.cpp:400`): the table's folder and its `user/` and `scripts/`,
then VPX's folders, then the preferences folder and its `user/` and `scripts/`.

- **The plugin installs the binding** into `<prefPath>/scripts/` at load, before any table script
  runs. It bundles a pinned `scorbit_sdk` release (tag and sha256, as it pins VPX's headers) and
  adds that release's file beside any already there. It never replaces or deletes one: an
  existing file of the same name has the same bytes.
- **A table must still run on a VPX without Scorbit.** Without the plugin there is no installed
  copy, so a table that calls the binding also ships its `scorbit-<version>.vbs` beside itself.
  Every call is then a silent no-op when `Scorbit.Table` cannot be created.
- The binding is not injected with `VPXPluginAPI::RunScript`: without the plugin the injected
  functions would not exist.

`Scorbit.Table` gains read-only `Version` (the plugin's) and `ApiLevel`. Plugins released before it
have no `ApiLevel` member at all, so the binding reads it under its own error guard: an object that
exists but has no `ApiLevel` is level 1 (the original verbs), distinct from no object at all (no
plugin). The binding states the level it needs; against a lower one it logs once and leaves the
missing feature a no-op. Native verbs are never removed or changed in meaning.

## Table Options

Gameplay settings that matter for fair comparison (balls per game, extra ball, difficulty, tilt,
rule toggles) belong in VPX Table Options, so the daemon can read them from the table's `.ini`
through the preferences path the table's declaration carries. The docs and the sample table do this.

That is not the whole answer. An option left at its default may have no entry in the `.ini`, and
VPX can be pointed at a different settings file, so the file alone cannot always give the
effective values. Reliable comparison needs the table to report its effective values, defaults
included, as they are at game start and when they change. That is `Scorbit.DeclareSetting`,
deferred until the protocol carries it; until then, comparison by Table Options is best effort.

## Tests

- **Binding** (`scorbit_sdk`): `cscript` tests on Windows against a VBScript mock of `Scorbit.Table`
  (through `ScorbitInitWith obj`), including the no-plugin path and, separately, an object with
  no `ApiLevel` (level 1), the argument translation, and a parity test comparing
  `GameState`'s game-facing methods with the binding's exports against an explicit list of the
  out-of-scope and commented-out calls.
- **Plugin:** unit tests for the install step (add the bundled release when missing, never replace
  or delete any file, never touch a table's own copy) and for the new properties.
- **Live:** an authored sample table in a live VPX, with and without the plugin enabled.
