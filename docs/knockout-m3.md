# Current integration: 0.3.7 / protocol 3

The M3 notes below are historical. The regular release now builds PAL with both
CPU and Knockout adapters, freezes the mode in an agreed public/private VS plan,
and relays ordered owner eliminations plus a canonical final finish base.
Human spectators and CPU retirement share the same grid. Native retirement
readiness retries separately from event acceptance, and pre-existing terminal
flags are preserved. Only one local human per console is supported for Knockout.

This integration is compiled only, with no automated or live tests at the user's
request. See the root README and docs/online-cpus.md. Previous candidate test
results do not establish live success for the combined release.

The user's 0.3.5 run reached elimination, but an unfinished spectator disconnected
about 35 seconds later, and the host later hit 91010 before in-game results.
Nested/NUL GP reports support a concurrent native-buffer write defect. Version
0.3.6 queues whole reports, flushing only after DWC_ProcessFriendsMatch through
one GP append per record. Pending finish/item state survives queue rejection.
Server framing also drains complete records ahead of partial TCP tails.

The native online VS AFK check still considers an unfinished eliminated human.
The active PAL path now resets its input idle counters via native leaf
`0x805235AC` using InputManager singleton `0x809BD70C`, scoped to an eliminated
unfinished local MASTER racer. Ordinary active-racer AFK and the race time cap
are preserved. KO placement state also survives native finishing stages 3/4.

The finish-notice opacity override did not activate its pane while the spectator
remained unfinished. A display-only `HasStarted` override at vtable entry
`0x808D410C` now enables the notice during elimination and delegates to native
`0x807F8D08` otherwise. Independent retail table review uses REL `.data` delta
`0x1731F0`, not the text delta below. Immediate HUD display, complete results,
connection retention and repeated races still need the user's manual check.

The 0.3.6 retry showed a separate pre-elimination gap: the guest left for AFK
before the first KO event and was never eliminated. Version 0.3.7 replaces the
eliminated-only counter reset with a scoped native input-idle predicate at
the audited 16-byte block `0x8053F238`. The shim replays argument construction,
returns to native comparison `0x8053F248`, and suppresses a true idle result
only for a negotiated KO race. A queued `wl:ko_afk` records actual suppression
once per controller/race. Ordinary VS, general disconnects and the separate
hard race limit remain native. This correction requires a new manual race.

---

# Knockout M3: retire-and-watch candidate

M2 is committed as `1f54847` (patcher), `dedb6cd` (server assets), and
`aba389d` (root notes). Its embedded 22972-byte report-only payloads remain the
production baseline. M3 changes are an experimental working tree.

## What changed after the first live test

The earlier `.phase0/knockout-m3/mkwfc-ko-m3` mechanics build immediately called
native `EndRace` with a synthetic `100:01.<placement>` finish time. The user
reported that the eliminated client saw **"You were disconnected from the
other players"**, while the other racer remained in the race. This establishes
a failed spectator outcome, not the exact disconnect cause.

The revised candidate stops the kart without declaring it finished, follows
a surviving racer with the existing camera, and shows its elimination placement.
It postpones native finish/results handling until the surviving racers finish.
**Connection retention, camera behavior, HUD rendering, and complete results
flow still require a new live test.**

A subsequent private-room test with the correct PAL disc patch stayed connected,
but the race ended as the leader started lap 2. Both clients reported
`wl:ko_apply`/`wl:ko_spectate` followed immediately by native finish reports,
without `wl:ko_result`. The native online update's handling of Vanish's `0x20`
flag explains that premature completion. The fix below clears that newly added
flag; its intended behavior still requires the surviving racer to finish the
full race while the eliminated player watches.

Normal builds remain report-only. `-DWWFC_KNOCKOUT_APPLY=1` enables active
behavior only for **PAL RMCPD00, private friend rooms, one local player per
console**. All participants must fully reboot onto the same candidate payload.
Worldwide/Continental races and unsupported regions remain report-only.

## Retail evidence and boundaries

The operator's PAL RVZ was read using decomp-toolkit v1.8.4 (upstream
<https://github.com/encounter/decomp-toolkit>). Only `sys/main.dol` and
`files/rel/StaticR.rel` were extracted to ignored `.phase0/knockout-m3/`.
No retail binary is checked into the patcher. `StaticR.rel` SHA-256:

`16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d`

The merged ELF was inspected with LLVM objdump. These are static observations,
not REA runtime evidence or observed spectator execution; REA's Ghidra provider
is unconfigured on this host. Merged REL `.text:1` starts at `0x8039D180`, versus
PAL runtime `0x805103B4`: add **`0x173234`** to merged REL text addresses. This
adjustment does not apply to main-DOL or arbitrary REL data/BSS addresses.
Pulsar/mkw-structures headers supplied ABI leads; no custom-mod knockout
implementation was copied.

### Retiring and following a racer

- Native `RaceinfoPlayer::Vanish` is PAL `0x80534C78`. It sets player
  `stateFlags@0x38 |= 0x20` and invokes native kart-stop logic. The examined
  chain does not set native finished (`0x2`) or disconnected (`0x10`) flags,
  increment either count, or call a disconnect function.
- Leaving `0x20` set is nevertheless unsafe online. In
  `GMDataOnlineVS::Update`, PAL `0x8053F354`–`0x8053F37C` (merged
  `0x803CC120`–`0x803CC148`) checks each player's `0x20` flag and, unless
  `0x10` is already set, calls `0x80533DD4`. That routine increments
  `Raceinfo+0x1D`, sets `0x10`, and evaluates native race completion. This
  race-level disconnect need not close the network connection; server room
  membership can remain active. With two racers it can also cause native
  automatic completion of the other racer.
- The candidate now captures Vanish's `0x20` success flag and immediately clears
  only that newly added bit before marking the custom elimination. The entry
  guard rejects a player whose `0x20` was already set. Other player flags and
  the native kart-stop effect are preserved. Custom state, rather than the
  native retirement flag, excludes eliminated racers from leader, candidate,
  and survivor selection; native updates and RH2 export see an unfinished racer.
  Live verification must still establish that the kart remains stopped and the
  spectator/results flow works after clearing this bit.
- Before Vanish, guards check KartManager singleton `0x809C18F8`, its player
  array/count (`0x20`/`0x24`), selected kart, kart pointers, movement pointer at
  `0x28`, movement pointers, and status at `0x04`.
- RaceCameraMgr is `0x809C19B8`; cameras are at `0x14`, count at `0x1C`, and
  focused camera index at `0x27`. The native camera update reads player ID
  `0x9C` each frame and resolves that kart.
- The candidate retargets existing camera 0 only when count is 1, focused index
  is0, and camera byte `0x342` is zero. That byte enables an alternate path
  indexing sorted cameras by player ID, so blindly retargeting it could go
  out of bounds. Single-local online VS construction was corroborated to use
  the normal zero-byte path.
- No `Raceinfo.isSpectating` or camera scene-level spectator flag is toggled.
  Those flags describe resources/network state established at scene creation;
  flipping them does not construct a spectator scene.

### Deferred finish and placement

- `RaceinfoPlayer::EndRace` is PAL `0x805347F4`. It sets finished bit `0x2`,
  copies the supplied timer, and internally calls `EndPlayerRace` at
  `0x8053491C`. **Never call `EndPlayerRace` separately:** it increments the
  native finished count.
- The symbol labeled `CheckEndRaceOnline` at `0x80533DD4` actually sets a
  disconnect flag/count in the inspected retail code. The payload does not
  call it, and clears Vanish's newly added `0x20` to prevent the native online
  update from calling it for a custom elimination.
- Retired racers are finalized only after all non-eliminated,
  non-disconnected survivors carry native finished flags. With multiple
  retired racers this explicit batch is necessary: native completion can keep
  waiting while more than one racer is unfinished.
- With one retired local racer, a natural survivor's finish may auto-finish it
  through native `UpdateLocalPlayers`. The existing mkwTime tail hook
  (`0x80534920`) detects that custom-retired racer and normalizes its timer
  only; it does not call EndRace or count it a second time.
- All retired results share a base at least as large as valid native elapsed
  time and survivor finish times. Disconnected racers, invalid clocks, and
  63-minute sentinel timers are excluded from that base. Their offset is `placement * 10ms`: earlier
  eliminations have larger final times and rank behind later eliminations.
  The helper rejects results that would reach63 minutes, so they fit the
  stock 6-bit minute field.
- The opt-in KO room path suppresses mkwTime's ordinary wall-clock correction
  for natural finishes. Otherwise an outer winner callback could be corrected
  after a nested retired callback and end up behind its already computed
  result. Ordinary report-only/non-KO behavior retains the original correction.

The old 100-minute workaround is no longer the active spectator strategy.
Stock RH2 export clamps times above 62 minutes to `(63,0,playerId)`; using that
old workaround while others are still racing was not a packet-safe finish
representation. The current change avoids that early finish path, but this
alone is not proof of the reported disconnect's root cause or its resolution.

### Placement notices

The immediate text is, for example:

```text
Eliminated - 4th place
Now spectating
```

- `mkwKnockoutUI.cpp` hooks the actual PAL finish-message control's full
  `Update` vtable slot, `0x808D40D0`, and calls native LayoutUIControl update
  first (`0x8063DB00`). It then applies the notice after native animation and
  opacity propagation, rather than locating a control through guessed HUD or
  menu array offsets.
- Guards check the expected vtable, HUD slot 0, layout root, text pane, loaded
  Common BMG, and that the native name finder returns the expected text pane.
- The finish control uses Race.bmg, which lacks generic template `0x19CA`.
  PAL race language archives also contain Menu.bmg, which does contain it.
  A zero-initialized, temporary BMG holder loads `Menu` with native
  `BMGHolder::Load` (`0x805F8B90`); native
  `MainLayout::GetTextPaneHandlerByName` (`0x805E9028`) finds `textBox_00`.
  `Text::PaneHandler::SetMessage` (`0x805CDD00`) formats the text and rebuilds
  MKW's glyph cache. Merely changing nw4r's raw string would not do that.
- The holder only views race-archive-owned data; the synchronous formatter
  does not retain its stack address. The notice text itself stays in a
  payload-owned buffer. Text/control/root opacity is applied after native
  updates so the ordinary unfinished-racer fade does not hide the notice.
- `WifiMenuPage::QueueNotice` stores a separate placement-only notice
  ("Eliminated - Nth place") for the next WFC-menu activation. The menu notice
  intentionally omits "Now spectating", since the player is no longer watching
  the race at that screen. It does not replace the ordinary server
  MOTD. `wl:motd` is still a login/menu feature, not a live server-to-race
  message channel. Returning only to the friend-room page need not trigger
  that WFC-menu popup yet.
- The user confirmed this queued popup appears after reconnecting through
  Nintendo WFC 1 Player on 2026-10-30, while the in-race notice did not appear.
  That report was still from v1, whose premature race completion can clear the
  race notice as the scene leaves RACE. The queue survives that reset. The v2
  full-race test must still verify the immediate HUD notice and its placement.

## Policy and tests

The implementation keeps M2's compatible `wl:ko_eliminate` record. The revised
active path emits `wl:ko_apply` and `wl:ko_spectate` after native retirement;
`wl:ko_result` identifies deferred result finalization. A report is not an
independent observation that the camera or network behaved correctly.

The host test builds the actual policy/notice headers with ASan/UBSan:

```sh
# Run from third_party/wfc-patcher-wii.
clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iinclude -Ipayload tests/knockout_state_test.cpp \
  -o /tmp/knockout-state-test
/tmp/knockout-state-test
```

Host regression cases passed for starting-line suppression, once-per-lap
selection, leader changes, invalid IDs/order/counts, reset, retired/finished/DC
exclusion, waiting for all survivors before finalization, stored placements,
packet-safe result bounds, invalid/sentinel clock rejection, ordinal
formatting, and bounded notice buffers.
These tests establish policy/formatting behavior, not native ABI execution,
rendering, connection retention, or peer agreement.

Build active PAL payloads with the existing key and domain:

```sh
# Run from the LanWFC root.
./tools/payload/build.sh --key ~/.mkwfc/payload-key.pem \
  --payload -- -gRMCPD00 '-DWWFC_DOMAIN="mkwfc.lan"' -DWWFC_KNOCKOUT_APPLY=1
```

The revised spectator build passed these checks on 2026-10-29:

- ASan/UBSan policy, notice-formatting, and clock-validation regressions.
- devkitPPC `-Wall -Werror` builds for all four retail report-only RMC regions,
  their channels, and the active PAL spectator payload.
- Presence of the native HUD `WritePointer` patch at `0x808D40D0` in the active
  payload; this build targets payload offset `0x36C4`.
- Embedded asset/manifest, signed-payload/catalog, bootstrap, and server tests
  using the spectator overlay.
- The experimental server executable build and first-run `init` in a disposable
  data directory.

The updated v2 candidate passed the host sanitizer regressions, active PAL
devkitPPC build, Go CLI/privbind/assets/payload/bootstrap/server checks, and a
disposable first-run initialization on 2026-10-30. Compiled code confirms the
flag clear/store immediately after the native call (`0x5288`–`0x5294` in the v2
payload ELF), before any following update can interpret `0x20` as retirement.

The active v2 PAL spectator payload is 27892 bytes, SHA-256:

`98d1ed5f8b953782a4df0295cf8b445aaaf93447f95148528c4baafb9f79756a`

`.phase0/knockout-m3/spectator-v2/build-info.json` records source/base hashes and
`live_spectator_validated: false`. These checks do not establish live camera,
message, connection-retention, or results behavior.

## New candidate and live retest

The built revised executable is
`.phase0/knockout-m3/mkwfc-ko-spectator-v2`, packaged with
`.phase0/knockout-m3/spectator-v2/go-overlay.json`. Production M2 asset files remain
unchanged. Normal generated PAL payload outputs were restored to report-only
after preserving the spectator candidate. The previous `mkwfc-ko-m3` executable is the earlier forced-finish
build that produced the user-reported disconnect. The first `mkwfc-ko-spectator`
kept the connection but ended the race at lap 2; use v2 for the spectator retest.

Stop the current server with Ctrl-C, stop Colima if it is still holding port 53,
then run:

```sh
cd /Users/zabur/Projects/LanWFC
./.phase0/knockout-m3/mkwfc-ko-spectator-v2 run --data-dir ~/.mkwfc
```

This replaces the running server for the test; it cannot coexist with another
instance using the same ports. Reusing the data directory preserves its keys,
certificates, accounts, and password. **Fully reboot both PAL games**, not just
WFC reconnect, then enter a private friend room with one local player on each
console.

For a PAL disc/RVZ, both Dolphin profiles must enable the generated
`~/.mkwfc/gecko/RMCPD00.txt` code. **Do not use `RMCPN0001`**, which targets the
Mario Kart Channel. The previous test setup used that Channel code in the disc
profiles: a saved guest state confirmed that it overwrote the private-join path
and branched into the middle of `DWCi_GT2ConnectAttemptCallback`, causing a DSI
at `0x8010A484` with a profile ID used as a connection pointer. Both profiles
were backed up, corrected to the disc code, and fully restarted on 2026-10-30.
That configuration correction requires a live private-join retest; it does not
establish spectator behavior.

The subsequent 2026-10-30 private-room test successfully joined both clients.
Both emitted `wl:ko_apply` and `wl:ko_spectate` with `131330` (lap 2, player 1,
two racers), and the next server snapshot still showed both active in the
private group. This verifies that the active retirement path was reached on
both clients; camera/HUD rendering, finish timing, stable placements, and
connection retention across repeated races still need on-screen confirmation.

Check both host/client roles:

1. Starting lap 1 causes no elimination; first retirement occurs when the leader
   starts lap 2.
2. The eliminated kart stops and its client stays connected while following a
   surviving racer; the other racer continues normally.
3. The race notice shows the correct placement/ordinal and remains readable.
4. Retired racers are not eliminated again. When surviving racers finish,
   retired results complete once and preserve placement order.
5. Results, WFC teardown, the queued WFC-menu notice, and a subsequent race are
   stable. The ordinary server MOTD is preserved.
6. Worldwide/Continental and non-PAL clients remain report-only.

For rollback, stop the candidate and run `./mkwfc run --data-dir ~/.mkwfc`, then
fully reboot both games again to discard the cached experimental payload.

## Still open

Host-authoritative decision/acknowledgment and placement agreement, mixed-client
checks, host migration/DC handling, cross-region verification, split-screen,
spectator controls/UI polish, and the complete live retest remain open. The
stock lap count also limits how many eliminations fit in one retail race;
continuous knockout/round rules are not implemented. CPU lobby filling is a
separate unchecked TODO requiring actual host-owned simulation and peer sync.
