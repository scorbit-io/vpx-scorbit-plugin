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
| `scorbit-<major>.vbs`, the binding | `scorbit_sdk` | The API authors use: SDK names, guarded, a no-op without the plugin |
| `Scorbit.Table`, the native object | this plugin | The contract the binding calls, versioned by `ApiLevel` |
| The daemon | Scorbit | Holds the SDK; answers the plugin over the local socket |

## Names

The SDK's own names with a `Scorbit` prefix: `ScorbitSetScore`, `ScorbitAddModeExpiring`,
`ScorbitSetModeCompleted`, `ScorbitGetStatus`, and constants as `SCORBIT_STATUS_…`. VBScript has one
flat global namespace, shared with `core.vbs`, the table's own names and any other plugin's script,
so unprefixed or shortened names would collide. Every routine has its own error guard, so nothing
in the binding can raise into a table.

## Feature parity

The binding covers the SDK's game-facing API:

| SDK (`GameState`, events) | `Scorbit.Table` today | In the binding |
|---|---|---|
| `setGameStarted`, `setGameFinished`, `setCurrentBall`, `setActivePlayer`, `setScore` (with feature), `addMode`, `addModeExpiring`, `removeMode`, `clearModes`, `commit` | yes, 1:1 | yes |
| Achievements: `setModeCompleted` (completed modes are matched against achievement conditions) | yes | yes |
| `getStatus`, `PairingStatusChanged` | no | yes, as properties: the plugin already receives the daemon's status, which carries unpaired, offline and degraded (`WireProtocol.h:471-474`) |
| Game status indication (in session, tracking, the reason when not) | no | yes, from the same status: session state and support text |
| `getMachineUuid` | no | yes: the uuid the table declared |
| `requestTopScores(scope, period, since, vpinFilter)` | no | yes, polled. Every argument as the SDK has it: scope machine, variant or game; period all time or 14 to 365 days; `vpinFilter` any (the documented default), virtual only or physical only. Replies are cached and capped (top N, names truncated) |
| `PlayersUpdated`, `player_info.h` | no | yes, polled. Per slot: the claimed player's name and id, or, when unclaimed, the claim deeplink, so a table can show a claim QR as a machine does |
| `PlayerPictureReady` | no | yes, polled, as a local file path valid for the session (usable through FlexDMD, for example) |
| `GameStartRequested` (start from the app) | no | **present, commented out** (below) |
| `setCreditsDropped`, `setCreditsStatus`, `PricingReceived`, `CreditsAddRequested`, `CreditsStatusRequested` | no | **present, commented out** (below) |
| Device plumbing (diagnostics, device state, probe firmware, downloads, `requestPairMachine`) | no | out: Scorbitron internals, not a game developer's API |
| `requestPairCode`, `requestUnpair`, `getPairDeeplink` | no | out: Scorbit Desktop and the plugin manage pairing |

VBScript handles asynchronous callbacks poorly, so every answer and event is **polled**: the
native object keeps the latest leaderboard, player list and events, and the binding exposes them
as properties and a `ScorbitPollEvent` loop the table calls from a timer.

Each new native feature has its own capability bit in the plugin's handshake with the daemon, so
a newer daemon with an older plugin, or the reverse, offers the features both have.

### Credits, pricing and starting from the app

The SDK's credits, pricing and remote-start calls apply to cabinets in public, coin-operated use.
They are in the binding, **commented out**, each with a comment saying what it does and that the
plugin does not serve it. Their names and signatures are reserved now, so they cannot clash with
anything added later.

Starting from the app also needs the table's cooperation: nothing outside VPX can press start on a
table, so a table that enables it starts its own game when it sees the request.

## Delivery and versions

The binding is versioned and never edited after release. Each major version is its own file,
`scorbit-<major>.vbs` (`scorbit-1.vbs` first), carrying `SCORBIT_SDK_VERSION`. Within a major only
compatible changes are made; a breaking change is a new major, installed beside the old one, so a
table always gets the API it was written for.

```vbscript
On Error Resume Next
ExecuteGlobal GetTextFile("scorbit-1.vbs")
On Error Goto 0
ScorbitInit "<machine uuid>"
```

`GetTextFile` resolves through VPX's script search (`FileLocator::SearchScript`, vpinball
`9a8f24cb6`, `src/core/FileLocator.cpp:400`): the table's folder and its `user/` and `scripts/`,
then VPX's folders, then the preferences folder and its `user/` and `scripts/`.

- **The plugin installs the binding** into `<prefPath>/scripts/` at load, before any table script
  runs. It bundles a pinned `scorbit_sdk` release (tag and sha256, as it pins VPX's headers) and
  replaces an installed copy only with a newer release of the same major.
- **A table must still run on a VPX without Scorbit.** Without the plugin there is no installed
  copy, so a table that calls the binding also ships the same `scorbit-<major>.vbs` beside itself;
  the file is identical to the installed one, and found first. Every call is then a silent no-op
  when `Scorbit.Table` cannot be created.
- The binding is not injected with `VPXPluginAPI::RunScript`: without the plugin the injected
  functions would not exist.

`Scorbit.Table` gains read-only `Version` (the plugin's) and `ApiLevel` (1 for the current verbs,
raised as features land). The binding states the level it needs; against a lower one it logs once
and leaves the missing feature a no-op. Native verbs are never removed or changed in meaning.

## Table Options

Gameplay settings that matter for fair comparison (balls per game, extra ball, difficulty, tilt,
rule toggles) belong in VPX Table Options, so the daemon reads them from the table's `.ini` through
the preferences path the table's declaration carries. The docs and the sample table do this.
`Scorbit.DeclareSetting` stays deferred.

## Tests

- **Binding** (`scorbit_sdk`): `cscript` tests on Windows against a VBScript mock of `Scorbit.Table`
  (through `ScorbitInitWith obj`), including the no-plugin path, and a parity test comparing
  `GameState`'s game-facing methods with the binding's exports against an explicit list of the
  out-of-scope and commented-out calls.
- **Plugin:** unit tests for the install step (install when missing, replace only with a newer
  release of the same major, never touch a table's own copy) and for the new properties.
- **Live:** an authored sample table in a live VPX, with and without the plugin enabled.
