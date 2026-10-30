# The Scorbit SDK for VBScript tables (design, SB-6010)

Status: design, for review. Nothing here is implemented yet.

## Decisions (2026-10-30)

- The VBScript layer is a **binding of `scorbit_sdk`**, beside its Python bindings, and lives in
  the `scorbit_sdk` repo (MIT, as the SDK is). It is documented in the SDK's own docs, as the same
  API with the same names, so a table author can do what a machine developer does.
- **It does not launch without feature parity** with the SDK's game-facing API (below). A
  reporting-only build may come first, for development; it is not released to authors.
- Pairing actions are out of scope: Scorbit Desktop and the plugin manage pairing. Pairing *status* is in.
- Delivery: the table loads the binding (the `core.vbs` pattern); no injection. See "Delivery".

## Why

In the old system a table reached Scorbit through internal calls into a third-party binary built
on the SDK. Now the plugin exposes the native `Scorbit.Table` object (#31), and the daemon holds
the real SDK: the plugin carries the table's calls to it over the local socket. The binding gives
authors the SDK's API on top of that object, without the boilerplate of creating and guarding it.

**No network in the table or the plugin.** The plugin makes no network connection (SB-5991). Every
call goes over the local socket to the daemon, whose embedded SDK talks to Scorbit; read-back
data (leaderboards, players, events) comes back the same way, and a player picture arrives as a
local file the daemon already downloaded.

## Three layers

| Layer | Repo | Role |
|---|---|---|
| `scorbit.vbs`, the VBScript binding | `scorbit_sdk` | The API authors use: SDK names, guarded, a no-op without the plugin |
| `Scorbit.Table`, the native object | `vpx-scorbit-plugin` | The contract the binding calls; versioned by `ApiLevel` |
| The wire and the daemon | plugin and daemon | Carries calls to the daemon's SDK and its answers back |

## Feature parity

| SDK (`GameState`, events) | `Scorbit.Table` today | Needs |
|---|---|---|
| `setGameStarted`, `setGameFinished`, `setCurrentBall`, `setActivePlayer`, `setScore` (with feature), `addMode`, `addModeExpiring`, `removeMode`, `clearModes`, `commit` | yes, 1:1 | nothing |
| Achievements: `setModeCompleted` (completed modes are matched against achievement conditions) | yes | nothing |
| `getStatus`, `PairingStatusChanged` | no | properties only, no wire change: the daemon's `Status`, which the plugin already receives, carries `SYSTEM_UNPAIRED`, `SYSTEM_OFFLINE` and `SYSTEM_DEGRADED` (`WireProtocol.h:471-474`) |
| Game status indication (in session, tracking, the reason when not) | no | the same record's session state (`SESSION_NONE`, `TRACKING`, `NO_DATA`) and support text, as properties |
| `getMachineUuid` | no | the uuid the table declared, echoed; no wire change |
| `requestTopScores` (`leaderboard.h`) | no | wire request and reply; polled from script |
| `PlayersUpdated`, `player_info.h` (claimed players' names) | no | wire event; polled |
| `PlayerPictureReady` | no | wire event carrying a picture the table can load (a file path); polled |
| `GameStartRequested` (start from the app) | no | wire event; polled |
| Credits and pricing (`setCredits…`, `PricingReceived`) | no | not applicable at home (free play); out unless asked |
| Device plumbing (diagnostics, device state, probe firmware, downloads, `requestPairMachine`) | no | Scorbitron internals, not a game developer's API; out |
| `requestPairCode`, `requestUnpair`, `getPairDeeplink` | no | out: pairing is managed by Scorbit Desktop and the plugin |

VBScript handles asynchronous callbacks poorly, so every answer and event is **polled**: the
native object keeps the latest leaderboard, player list and events, and the binding exposes them
as properties and a `ScorbitPollEvent` loop the table calls from a timer.

## Delivery: the table loads it

```vbscript
On Error Resume Next
ExecuteGlobal GetTextFile("scorbit.vbs")
On Error Goto 0
ScorbitInit "<machine uuid>"
```

`GetTextFile` resolves through VPX's script search (`FileLocator::SearchScript`, vpinball
`9a8f24cb6`, `src/core/FileLocator.cpp:400`): the table's folder and its `user/` and `scripts/`,
then VPX's folders, then the preferences folder and its `user/` and `scripts/`.

- **Authors ship `scorbit.vbs` beside the table**, so it is found first and the table always has
  the binding it was written against, plugin or not.
- **The plugin installs a copy** to `<prefPath>/scripts/scorbit.vbs` at load, before the table
  script runs, replacing an older one only. It bundles a pinned `scorbit_sdk` release, as it pins
  VPX's headers.
- **A table must still run on a VPX without Scorbit**, so every binding call is a silent no-op
  when `Scorbit.Table` cannot be created, and never raises. Injection (`VPXPluginAPI::RunScript`)
  is not used: without the plugin the injected functions would not exist.

## Versions

- `scorbit.vbs` carries `SCORBIT_SDK_VERSION` (the SDK release's) and the `ApiLevel` it needs.
- `Scorbit.Table` gains read-only `Version` (the plugin's) and `ApiLevel` (1 for the #31 verbs,
  raised as each parity row lands). On a lower level the binding logs once and leaves the missing
  feature a no-op.
- Native verbs are never removed or changed in meaning, so an old binding beside an old table keeps
  working against a newer plugin.

## Table Options (the original SB-6010 ask)

Gameplay settings that matter for fairness (balls per game, extra ball, difficulty, tilt, rule
toggles) belong in VPX Table Options, so the daemon reads them from the table's `.ini` through the
prefs path the Declare carries. The docs and the sample table do this. `Scorbit.DeclareSetting`
stays deferred: it needs a wire message.

## Work, by repo

1. **`scorbit_sdk`:** `wrappers/vbscript/scorbit.vbs`, its docs in the SDK's, and `cscript` tests on
   Windows CI against a VBScript mock of `Scorbit.Table` (the binding accepts one through
   `ScorbitInitWith obj`), including the no-plugin path.
2. **`vpx-scorbit-plugin`:** `Version`, `ApiLevel` and `Status` properties; the polled stores for
   leaderboards, players, pictures and events; the `<prefPath>/scripts` install; the Table Options
   docs.
3. **The daemon:** wire messages for leaderboards, players, pictures and start requests, and the
   pairing state in `Status` if it is not there.

## Open for review

1. Function names in the binding: the SDK's own, prefixed (`ScorbitSetScore`), or shorter forms.
2. Whether the plugin should install to `<prefPath>/scripts` at all, or leave delivery to authors.
3. Credits and pricing at home: confirm they stay out.
