# Slice 4: plugin internals

How the plugin delivers the daemon's DMD overlay to every consumer, and how its threads
hand data to each other without waiting. The wire contract lives in scorbitd's SPEC-0002
design, "Message layouts (protocol 1.0, slice 4)". Section references such as REQ-x.y are to
scorbitd SPEC-0002.

## Delivery downstream

**Delivery downstream** (REQ-4.3). Cabinets drive real DMDs from VPX, so the overlay MUST reach
everything that shows the DMD as if it were original data: VPX's own DMD, the PUP and B2S DMD
overlays, ScoreView, and DMDUtil (ZeDMD, Pin2DMD, Pixelcade).

The plugin bus has no central override rule; each consumer picks its own source:
- **DMDUtil** (`SelectSource`) takes the widest non-LUM32F source, and the first in list order on a
  tie. It ignores `overrideId`, so it reaches Serum's output through that preference alone.
- **VPX's default** (`GetDefaultDisplaySource`) ranks by pixel count first and prefers colour only
  on a tie.
- **VPX's DMD, PUP, B2S and ScoreView** then walk the override chain from that default through
  `ResURIResolver`.

The override therefore follows these rules (`src/DmdOverlay.*`, `src/OverrideChain.h`):

- **List position.** The plugin answers `CTLPI_DISPLAY_GET_SRC_MSG` from its own subscription,
  made at plugin load and released only at unload:
  - It answers no entries until it has something to override, so it is listed ahead of
    libpinmame, which subscribes at game start and again at the tail on every re-publish.
    `CtrlItemProvider` cannot hold this position: `ClearItems` unsubscribes, so every `SetItem`
    re-subscribes at the tail.
  - A change still takes two `ON_SRC_CHG` broadcasts, the first with the entries withdrawn and the
    second with the new ones. Consumers must drop the old `DisplaySrcId` before its geometry or
    buffers change.
  - Both broadcasts are deferred to the next main-thread turn. A source change arrives inside
    another plugin's broadcast, a controller teardown among them, and a colouriser that restarts
    its thread on our change there deadlocks joining it. Until then, an entry whose target has gone
    serves its own buffers and never calls the target again.
  - Consumers that take the first match then pick the override: DMDUtil, UpscaleDMD and VPX's
    default on a tie. Serum, VNI and PUP take the last match and keep PinMAME's clean source.
- **Uncoloured table:** overrides PinMAME's display in **LUM32F**. The output is PinMAME's frame
  with the overlay's luminance merged, so every consumer applies its own tint exactly as for the
  game. A colour format is not used: VPX tints only `BW_FP32` frames (`Renderer::SetupDMDRender`)
  and DMDUtil only LUM32F (`LumTint`), so a colour override would recolour the DMD for the whole
  game.
- **Coloured table (Serum, VNI):**
  - The plugin overrides **every** colouriser output, each at its own size and format, scaling the
    placement to match. Serum v2 publishes 32- and 64-row SRGB565 outputs.
  - **Never a sibling.** After every `ON_SRC_CHG` the plugin leaves the override chain from the
    controller unbranched, with its own override at every leaf. When another source overrides the
    same parent as ours, our override moves below it, never beside it. `ResURIResolver` takes the
    first override of a source in list order, so a link that starts at the controller (a flasher
    set to `ctrl://pinmame/display`) would otherwise stop at our sibling and lose the colours. The
    default link starts from the largest source and is not affected.
  - **Early order** (the normal case): Serum takes the last match, so it binds to PinMAME's own
    source directly. The plugin withdraws its PinMAME override and overrides each colouriser
    output. Serum is not disturbed, because its input was never ours.
  - **Late order** (seen on the rig before early subscription existed): Serum chains onto our
    PinMAME override. The plugin keeps that override and also overrides each colouriser output.
  - To withdraw, the plugin answers without the entry and keeps its subscription, so it keeps its
    place in the list.
  - The overlay is composited over the coloured frame. The colouriser keeps colouring the clean
    game frames, and DMDUtil picks ours on the equal-width tie.
  - The overlay's colour there is white × luminance, encoded to sRGB.
- **Identify frames:**
  - On a PinMAME override they are passed through unchanged.
  - An override of a colouriser's output does not expose `GetIdentifyFrame`. Serum and VNI take the
    last source with an identify frame that traces back to the controller, so they could select
    ours as their input. That forms an override cycle, and `ResURIResolver`'s chain walk never
    terminates, which hangs VPX.
  - Colourisers and PUP triggers keep reading PinMAME's original identify frames, either directly
    or through the PinMAME override's pass-through.
- **Extraction:** `DmdTap` keeps binding to PinMAME's own source, so it never sees the overlay.

**Side channel (later, opt-in).** The plugin MAY add a `Scorbit/OverlayState:1` bus event and a
`Scorbit/GetOverlayState:1` query, carrying the current bitmap, placement and visibility. They
would be broadcast on VPX's main thread, and the struct would go in `ScorbitPluginAPI.h`. With them
a colouriser or DMDUtil could colour the overlay itself, or ask the plugin to. Nothing depends on
either message being adopted, and neither exists yet.

Script-fed DMDs (`RawDmdPixels`) read PinMAME directly and get no overlay. DMDExt receives frames
only through the Windows VPinMAME `DmdDevice.dll` path, outside the plugin bus; it is handled last,
with its maintainer.

## Handoffs between threads

None of these makes the API or render thread wait on the socket worker (REQ-8.3).

- *Declare latch*, API thread → worker (`src/Latch.h`): the *Status latch*'s mechanism in the
  other direction.
  - The slot is an atomic pointer to an immutable, heap-allocated declaration plus an epoch.
  - The API thread builds the declaration once per rendered frame, on table start and end, on a
    ROM change, and from the plugin's quarter-second timer, so a display change while no frame
    renders is still seen. When it differs from the last one posted, the API thread
    `exchange`s the new record in and deletes the one it displaced.
  - On every pass of its loop, the worker `exchange`s in null. If it took a record, that becomes
    its private *current* declaration, replacing the previous one.
  - It sends a `Declare` whenever *current* differs from its private *last sent*.
  - On every new connection the worker clears *last sent*. *Current* is kept, so the latest
    declaration is re-sent after `HelloAck` even though the latch is empty.
  - Neither side waits on the other, so `ON_SRC_CHG` never blocks on the worker. It is bounded at
    one record by construction. A record left in either latch at unload is freed with it.
- *Overlay cache*, writers → render-frame providers (`src/OverlayCache.h`): a seqlock over one
  fixed-size record.
  - The record holds width, height, up to 192×64 pixel bytes, the enable bit and the hide
    deadline as a steady-clock time. It contains no pointers or strings.
  - **Two writers serialise on a writer-only mutex that readers never take:**
    - the socket worker: uploads, control, the timer restart, and the clear at session end;
    - the API thread's in-process `Scorbit/Overlay` message, which the file-drop demo and other
      plugins use.
  - **The API thread never waits on the worker.** It takes the writer mutex only with `try_lock`.
    - If the worker holds the mutex, the op goes into a bounded pending queue of 8 ops, owned by
      the API thread.
    - **While the queue is non-empty, every new op is appended to it** rather than trying the
      lock. Ops therefore apply in arrival order, which upload-then-show depends on.
    - The API thread drains the queue in order, again with `try_lock`, at its next
      `OnPrepareFrame` and from the plugin's quarter-second timer. A drain that cannot take the
      lock leaves the queue for the next one.
    - A queued show's timer counts from its arrival, not from the drain.
    - The timer drain matters while no frame renders: VPX's own DMD is not drawn while paused,
      but DMDUtil keeps reading the override on its own thread and drives the real display.
    - An upload is queued with its pixels only, after checking for room, so a trailer the
      payload carries costs the API thread nothing.
    - The bus reply's `applied` means *accepted*, not *shown*. It is decided from the payload
      alone (size bounds, and whether a surface exists) and does not wait for the write. A queued
      op answers `applied = 1`; only an invalid payload or a full queue answers 0.
    - Only the worker ever blocks on the mutex, and only for the bounded time of the API thread's
      copy into the record.
  - `GetRenderFrame` runs on any consumer thread, and several consumers may read at once. Each
    override keeps the last sequence number it saw and copies the record only when that changes.
    It judges visibility against the deadline on every call, and never waits on a writer.
  - A reader that retries more than three times composites from its own last good copy for that
    frame.
- *Snapshot*, API thread → worker (`MemoryBridge`): the memory ranges a `Poll` serves, published
  once per frame through the same seqlock and tagged with the subscription they were read for, so
  a `Poll` never serves bytes of a set since replaced. A `Poll` that sees the snapshot torn more
  than three times answers `busy`. A game ending publishes an empty snapshot, so a restart cannot
  serve the last game's bytes.
- *DMD frames*, tap worker → readers (`DmdTap`): each captured identify frame is published through
  the same seqlock.
- *Status latch*, worker → API thread (`src/StatusNotice.h`): `PushNotification` and
  `UpdateNotification` are plugin API calls and run only on the API thread (REQ-8.1), so the
  worker answers `Status` and stores the decoded record in a single slot, overwriting whatever is
  there. The slot is an atomic pointer to an immutable, heap-allocated record. The worker builds
  a new record, `exchange`s it in and deletes the one it displaced; the API thread `exchange`s in
  null and owns what it took. Neither side ever reads a record the other can still write. A
  seqlock would not do here: the record holds `support_text`, and overlapping a copy with a
  rewrite that frees its storage is undefined behaviour before any sequence check can run.
  It is bounded by construction. Losing an older state is correct because REQ-6.3 asks for the
  latest state, observed at most one frame after it changes: a state overwritten before the next
  drain lasted less than a frame, which REQ-6.3 does not require to be shown, and the log still
  has it because the worker logs each `Status` as it decodes it. A queue would only replay toasts
  nobody could read. The API thread exchanges the slot once per frame in `OnPrepareFrame` and, if
  it took a record, posts or updates the notification there. The plugin's own no-session notice
  (REQ-6.11) is posted from the same drain. `PushNotification` returns 0 and does nothing outside
  a running game; that is acceptable, because `OnPrepareFrame` runs only in a game.

  The per-frame drain is deliberate, and a drain that keeps running while frames do not render
  would be wrong. VPX draws notifications in its live UI, inside rendered frames, and times each
  one from the moment it is pushed (`NotificationOverlay.cpp`). A notification pushed while no
  frame renders, because the game is paused or VPX is unfocused, would therefore expire unseen.
  The latch holds the latest state until frames resume, and the notification then shows for its
  full length. The no-session notice's 5 s is counted from the loss of the session, so it posts
  on the first frame after that.

**The seqlock** (`src/SeqLock.h`), which the overlay cache, the snapshot and `DmdTap` share: the
buffer is stored as `std::atomic<uint64_t>` words, written and read with relaxed operations
between the sequence counter's release and acquire. A reader that overlaps a write therefore
copies a torn value without a data race, sees the counter move, and discards it. A plain `memcpy`
racing the writer would be a data race and undefined behaviour, even though the sequence check
would catch the tear. Writers are serialised by their owner. A reader that finds a write in
progress yields until it ends, bounded, rather than spending its retries at once: a copy takes
the writer about a microsecond, so retries spent immediately would all fail. A frame reader that
still gives up keeps the frame it already held, so a tear is never mistaken for a display that
went away.

**Withdrawn overrides** are kept for a second after the broadcast that withdrew them before they
are freed. Consumers drop them inside that broadcast, by the `CtrlItemConsumer` contract; the
margin is for one that does not.
