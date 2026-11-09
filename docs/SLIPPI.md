# Slippi on melee-nx

Updated November 9, 2026. **Online play is built and deployed but has not been played yet.** A
Direct match against Slippi Dolphin is the next hardware test; until it passes, nothing here claims
compatibility with Slippi Dolphin.

## Playing online (Direct mode)

1. Sign in once in the [launcher](LAUNCHER.md#slippi).
2. On the launcher's Play page pick **Slippi** and press A twice. The launcher asks for the
   opponent's connect code (`ABCD#123`) and remembers it.
3. In the game, open **VS Mode > Melee**, pick a character on any port, press Start, pick a stage.
4. The game shows "Looking for ABCD#123" until the opponent searches for your code in Slippi
   Dolphin's Direct mode, then "waiting for their pick" until they lock in. B cancels and returns
   to the character select.
5. The match starts on both sides. Afterwards the game returns to the character select; picking
   again plays the same opponent on the open connection.

Vanilla mode, and every game mode other than VS Mode > Melee, never touches the network.

What differs from Slippi Dolphin's menus:

- **No Slippi menus.** They need Slippi's own disc files (`SlippiCSS.dat` and others), which a
  retail disc does not have. The opponent's code comes from the launcher, and VS mode's own
  character and stage select make the picks.
- **Stage.** This game always sends its stage as a pick. Both games play the first picked stage in
  port order, which is how Slippi Dolphin reads the same messages, so the two always agree; when
  the Dolphin player is the lower port and has picked (or drawn a random stage on the first game),
  theirs is played.
- **Direct only.** Unranked, Ranked and Teams are not offered. An untested client should not be
  matched with strangers.
- **No chat, no name tags, no replays, no game report.**
- **Version gate.** If `user.json` names a Slippi version newer than `3.6.4`, the search stops with
  "Slippi x.y.z is out. Update Melee NX", as Slippi Dolphin refuses to search when outdated.
- **Delay** is 2 frames; `slippi_delay` in `perf.cfg` sets 1 to 9.

## Status

| Piece | State | Where |
|---|---|---|
| Account (`user.json`): validate, store, sign out | Done, host-tested | `launcher/source/data/slippi_account.*` |
| Sign-in from a phone or PC on the local network | Done, host-tested (parser and handler) | `launcher/source/data/lan_signin.*` |
| Wire formats, matchmaking messages | Ported, host-tested | `switch/slippi/slippi_protocol.*` |
| Matchmaking and netplay clients, ENet for libnx | Ported, tested over loopback; `mm.slippi.gg` not contacted yet | `switch/slippi/slippi_net.*` |
| Host commands (inputs, savestates, match state, selections) and the Direct lobby | Ported, tested over loopback against a scripted peer | `switch/slippi/slippi_host.*` |
| Rollback snapshots | Deterministic on hardware (25/25 gameplay checksums) with full copies; the faster copy (used heap ranges, 3 cores) is unmeasured | `switch/src/rollback_switch.*` |
| Game side: pad exchange, prediction, rollback, checksums, time sync | Ported, builds, not run | `switch/slippi/game/slippi_online.c` |
| Slippi's gameplay codes (UCF 0.84, neutral spawns, determinism fixes) | Ported, builds, not run | `switch/slippi/game/slippi_codes.c`, hook sites |
| Slippi's online menus, chat, name tags | Not ported (need Slippi's disc files) | donor `mu_slippi_*.c` |
| Sound reconciliation during rollback | Not ported; re-simulated frames are silent | donor `mu_online_audio.c` |
| Replays (`.slp`), game report | Not ported | donor `mu_replay*.c`, `slippi_report.cpp` |
## Donor

The donor is [Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked), checked out at
`ref/melee-unlocked`. It was reviewed at `f5c5cad0` on October 24 and re-inspected at
`6d5e2e4bb78a89c01d74b86e0b20057f2d28f7ca` on November 8. Its Source Port compiles the same
doldecomp Melee sources that melee-pc uses, so its game-side Slippi code is written against
functions and structures melee-nx also has. Its README says it plays against regular Slippi Dolphin
clients. This project has not checked that claim.

The donor can't be used as melee-nx's engine as it stands:

- Its Source Port is a GCC library for x86-64 Windows/Linux, loaded by a Windows host.
- The host renders by interpreting the game's GX FIFO with its own D3D12/D3D11 renderer. Aurora is
  listed as a submodule but not used.

melee-nx keeps melee-pc, Aurora and Dawn, which run on hardware, and brings the donor's Slippi
pieces across one layer at a time. Donor files are GPL-2.0-or-later, which can be used under
melee-nx's GPL-3.0-or-later. Keep their attribution. Never vendor game source or disc-derived
data.

## What was ported, and how

`switch/slippi/slippi_protocol.*` takes the following from the donor's `slippi_net.{h,cpp}` and
`slippi_netplay_receive.h`. It has no sockets, threads or platform calls, so the same code builds
for the Switch and runs in host tests.

- **Packet**: SFML `sf::Packet` layout. Integers are big-endian, a bool is one byte and a string is
  a u32 length plus bytes. A read past the end fails and leaves the target unchanged.
- **Pad messages**: `0x80`, then frame, port, checksum frame and checksum (14 bytes), then 8 bytes
  of input per frame, newest first.
  - The receive rule matches Slippi Dolphin. A frame is added only if it is newer than the queue
    head. More than 128 frames is refused, and a packet shorter than its frame count is refused.
  - The ack message is `0x81`, frame, port.
- **Match selections** (`0x82`): field order exactly as the donor writes it. Quick-chat ids are
  checked against the same allow-list.
- **create-ticket**: the JSON request to `mm.slippi.gg:43113`. It holds the account (with the play
  key, so never log it), the mode, the Direct code as a byte array, `appVersion` (`3.6.4`, the
  donor's value) and the LAN address.
- **get-ticket-resp**: read with type checks and refused whole unless all of these hold:
  - 2 to 4 players
  - every port 1 to 4 and used once
  - exactly one local player

  The parser picks the LAN address when the peer shares the local external IP. It also applies the
  default stage list. Server `error` and `latestVersion` are surfaced.
- `ConnectCodeForGame` converts a connect code to the game's name-tag text.

Left out on purpose:

- **YouGame**: ranked lobbies and its socket channel.
- **The donor's extensions**: early inputs, build and ping messages, shield colours. Slippi Dolphin
  ignores these message ids.
- **Test hooks.**
- **Windows code-page conversion**: `convert_string_for_game` needs a UTF-8 to Shift-JIS table on
  Switch.

ENet is the donor's vendored copy (MIT). It is built without `yg_enet_socket.c`, the YouGame
channel, which needs mmap and POSIX file descriptors. `native_socket_only.c` reports "never
selected", so every call takes ENet's own UDP path. `include/enet/unix.h` gets `<arpa/inet.h>` on
`__SWITCH__`.

To check it:

```powershell
powershell -ExecutionPolicy Bypass -File switch\slippi\tests\run-tests.ps1
```

This runs three suites (the third is described under [Checking it](#checking-it)):

- **Protocol (40 checks):** the packet layout, pad round trip and refusal cases, selections,
  create-ticket and hostile tickets.
- **Network (22 checks), real ENet over 127.0.0.1** using ENet's win32 backend:
  - two netplay clients connect and exchange selections, 20 frames of pads with checksums, acks
    and quick chat
  - dropping finalised inputs
  - disconnect detection, and the 8-second connect timeout when nobody answers
  - two `Matchmaking` instances pairing through local peering and handing over connected clients
  - the version gate


ENet, the protocol, the clients and the host commands are part of the game build
(`switch/CMakeLists.txt`).
### `slippi_net` compared with the donor

`slippi_net.*` ports the donor's `NetplayClient` and `Matchmaking`:

- **Kept:** connection slots by exact address, then host; replacing a reconnected peer; the
  unsequenced pad and ack channel; resending everything the slowest peer has not acked, capped at
  128 frames; ping from acks; Slippi's middle-third time-offset average; rematch selections
  clearing stale pads; the create-ticket and get-ticket flow with a new search after a failed
  connection.
- **Dropped:** YouGame, early inputs, the donor's build and ping messages, network-lag
  simulation, test hooks, the chat-disabled reply and the HTTP session version check. The
  version check is replaced by comparing `user.json`'s `latestVersion` with `kSlippiSemver`.
- **Changed:** pads are a `std::deque<Pad>` (newest first) instead of `unique_ptr` queues, decoded
  by `slippi_protocol`'s `DecodePads`. Log lines go through `SetLogger`.
- **Vendored ENet edits:** `win32.c` gets ENet's public socket names back (the donor renamed them
  for its YouGame layer), and `unix.h` declares `HAS_SOCKLEN_T` on `__SWITCH__`.


## How online play is built

Three layers, as in Melee Unlocked's Source Port:

- **Host** (`switch/slippi/slippi_host.cpp`, C++): answers Slippi's commands the way Slippi
  Dolphin's EXI device does, on top of `slippi_net` and the snapshot slots
  (`melee_nx::rollback::SlotCapture`/`SlotLoad`). Reply layouts are byte for byte the donor's.
  It also runs the Direct lobby and owns the status line.
- **Game, outside snapshots** (`switch/slippi/game/slippi_online.c`, built into
  `melee_game_host`): the port of the donor's `mu_online.c`. A rollback load restores the game,
  never the record of which frames were predicted or confirmed.
- **Game, inside snapshots** (`switch/slippi/game/slippi_codes.c`, built into `melee_game`): the
  gameplay codes that keep state, such as UCF's pad ring.

Every hook is inert unless `melee_nx_online_codes()` is nonzero, which is only while an online
match is pending or running.

### The match flow

| Step | Where | What happens |
|---|---|---|
| Stage select is left | `gmVsMelee_ExitSss` | `melee_nx_online_lobby`: search (`CMD_FIND_OPPONENT`), send the picks once connected, wait until both players are ready. The loop presents frames itself (`VIWaitForRetrace`) with the last frame held and the status line on top. The negotiated game info block replaces VS mode's match, and the preload cache and sound banks are set for it. |
| Match starts | `fn_8016E730` | `melee_nx_online_start_melee` (InitOnlinePlay): applies the block again, seeds the RNG, adds the per-frame RNG sync process, sets delay and ports. |
| Every pad sample | `HSD_PadRenewRawStatus` | `melee_nx_online_pad_renew` (TriggerSendInput): sends the local pad for frame N + delay, takes the remote pads, predicts missing ones, decides on a rollback, or drops the sample while the two games wait for each other. |
| Every engine body | `gm_801A4D34` | `melee_nx_online_frame_begin` (StartEngineLoop): loads the savestate when a rollback starts, computes and compares checksums, captures a savestate while a remote input is predicted. `melee_nx_online_frame_end` (LoopEngineForRollback) re-runs bodies without drawing until the rollback has caught up. |
| Match ends | `gm_Scene_Vs_OnExit`, `gmVsMelee_ExitVs` | Statistics go to the log; VS mode returns to the character select (no results screen, no sudden death). |
| VS mode is left | `gmVsMelee_ExitCss` | The connection is closed. |

A disconnect or a checksum mismatch ends the match the way L+R+A+Start does and shows
"Disconnected" or "Desync detected".

### Slippi's gameplay codes

Slippi Dolphin always plays with these, so an online match here does too. Each is a hunk in the
patched sources, ported from the donor's `melee-native.patch`.

| Code | Site |
|---|---|
| UCF 0.84: pad buffer and 1.0 cardinals, dashback, crouch dashback, SDI, shield SDI, shield drop, shield drop extended, tumble wiggle | `fighter.c`, `ftCo_Turn.c`, `ftCo_SquatRv.c`, `ftCo_Damage.c`, `ftCo_Guard.c`, `ftCo_Escape.c`, `ftCo_Pass.c`, `ftCo_DamageFall.c` |
| NeutralSpawn | `gmvs.c` |
| Init Player Data, Init Stage Data | `fighter.c`, `ground.c` |
| BrawlOffscreenDamage | `fighter.c` |
| FreezeDeadUpFallPhysics | `ft_0D31.c`, `ftdrawcommon.c` |
| DynamicsFix | `ftdynamics.c` |
| WhispyBlowDirFix | `ftlib.c` |
| FreezeGlitchFix | `ftnanaspecials.c` |
| DesyncProofBGTransformations (Final Destination) | `grlast.c` |
| Pokemon Stadium: frozen check, synchronous file load, GrPsxIsValid, camera-independent monitor | `grpstadium.c` |
| PreventCharacterCrowdChants | `crowdsfx.c` |
| SyncRNG (seed from the frame number and the match's offset) | `slippi_online.c` |

Not ported, each a possible desync until checked against Slippi Dolphin:

- **PreventWobbling.** Slippi applies it in Ranked and Unranked; this project's reading is that
  Direct allows wobbling, which is unverified. An Ice Climbers grab is the case to watch.
- **Pokemon Stadium's zero-buffer code** and anything Slippi added after the donor's inspection.
- **Teams and the costume bound check**, which Direct singles does not reach.

### Differences from the donor's `mu_online.c`

- **Dropped:** its test harness and self-test, early inputs, tracing, the correction-size
  statistics, replay recording, the game report and the HUD text (the host draws the status line).
- **Sound.** The donor reconciles sounds across a rollback (`mu_online_audio.c`). Here
  `AXDriver_8038CFF4` starts no sound during a re-simulated frame. A sound whose start a rollback
  corrects is lost, and one a wrong prediction started keeps playing.
- **Alarms.** `pc_os_alarms_hold`/`_release` (in `src/pc/os.c`) keep every OS alarm's schedule
  through a snapshot load, inside `rollback_switch.cpp`'s `Restore`. Without it the 60 Hz pad
  alarm, whose `OSAlarm` lives in game memory, would fire again for every frame a load went back.
- **Waiting.** The engine loop's pad wait sleeps 0.3 ms per turn during an online match instead of
  spinning.

### Checking it

```powershell
powershell -ExecutionPolicy Bypass -File switch\slippi\tests\run-tests.ps1
```

The third suite (267 checks) drives `mu_slippi_command` and the lobby as the game would, against a
scripted remote player (a bare `NetplayClient` on 127.0.0.1): the lobby, the match state reply
and game info block, 60 frames of pad exchange in both directions, the savestate commands, a
rematch on the open connection, and a disconnect.

There is no test of the game-side loop or the gameplay codes off the console. They compile and
link; what they do is known only from reading them against the donor.

### First hardware test

Play Direct against Slippi Dolphin on the PC, then pull `melee-nx-runtime.log`. The lines to read:

| Line | Meaning |
|---|---|
| `[slippi] search started`, `[slippi net] netplay: connected` | Matchmaking and the peer connection worked. |
| `[slippi] match setup stage … rng … P1 char … P2 char …` | The match both games agreed on. |
| `[slippi game] online: match starts, local port …, delay …` | The match scene took the negotiated data. |
| `online: skip starts/ends at frame …` | The games waited for each other (time sync, or a slow start). |
| `online: desync risk`, `online: DESYNC at frame … (ours …, theirs …)` | Checksums differ: a gameplay code is missing or wrong. The frame number is where to look. |
| `online: match exit: N rollbacks (deepest …), … re-simulated frames` | Rollback activity. |
| `online: rollback time: longest … us, average … us, N over a frame` | Whether snapshot load plus re-simulation fits in a frame. |

Known risks for that test:

- **Slow start.** Slippi Dolphin gives up after about 7 seconds without the first inputs. Loading
  the match on the Switch has to finish within that of the PC reaching the match.
- **Snapshot cost.** A savestate is captured on every frame that runs with a predicted input. The
  full copy measured about 30 ms; the faster copy is unmeasured.
- **Any difference in the game info block or a gameplay code** shows up as a desync, usually
  within the first seconds.

Record the NRO hash with any result, and compare the downloaded deployed NRO first.
## Rollback self-test

Rollback needs three things: the game's whole simulation state can be saved and restored, frames
re-run from a restored state come out the same, and both are fast enough at 60 FPS. The self-test
checks all three on the console.

**What a snapshot covers.** This follows the donor's `native_savestate`:

- **MEM1:** Aurora's emulated 96 MB block, which holds every game heap.
- **The game's own globals:** the `melee_game` library's `.data` and `.bss`.
  `switch/CMakeLists.txt` builds that library with one data and one bss section per object
  (`-fno-data-sections`), then renames them to `mgdata`/`mgbss` after archiving. The linker
  brackets them with `__start_`/`__stop_` symbols, so no Aurora, Dawn, SDL, libnx or newlib state
  is included.
- **Left out:** ARAM (sound samples and file staging), the audio heap, and the objects built as
  `melee_game_host` (audio, render caches, memory card, debug, and the online bookkeeping).

**The hooks.** `melee-switch-rollback-hooks.patch` adds three calls to the game's engine loop
(`gm_801A4D34` in `gmscene.c`), where the donor hooks Slippi's rollback:

- `melee_nx_rb_body_begin` at the start of each engine body.
- `melee_nx_rb_pads` at the pad renew point. A re-simulated body skips `lb_800198E0` and gets the
  pad library and master status recorded for that frame instead.
- `melee_nx_rb_body_end` at the end of each body. Returning 1 runs the next body at once, without
  drawing, as the donor's `LoopEngineForRollback` does.

A snapshot is only taken or restored when no disc request is in flight (`HSD_DevComIsBusy` on all
four queues) and the scene is not ending. Otherwise the test is skipped.

**Running it.** Put `rollback_selftest 600` in `sdmc:/switch/melee-nx/perf.cfg` (frames between
tests; 0 or absent is off, the default). `rollback_selftest_depth` sets the frames re-simulated,
7 by default, the most Slippi rolls back. Then play a match. Each test:

1. copies the snapshot regions
2. plays the next *depth* frames normally, recording each frame's pads
3. hashes the state per 4 KB page
4. restores the copy
5. re-runs the same frames with the recorded pads inside one host frame
6. compares the page hashes

Each test logs one line in `melee-nx-runtime.log`:

```text
[rollback] self-test 3 at frame 1800: match; capture 9.81 ms, restore 9.44 ms, resim 7 frames 21.30 ms, hash 18.02 ms; 3/3 passed
```

A mismatch lists the differing pages as `mem1+0x…`, `mgdata+0x…` or `mgbss+0x…`. `mgdata` and
`mgbss` offsets map to symbols with the matching `melee.elf` (`__start_mgdata` plus the offset).

**Expected side effects.** The test causes a visible hitch each time it runs: it copies about
100 MB twice and hashes it twice. Sounds in the re-simulated frames play again. With the key
absent, the hooks return at once and nothing changes.

**First hardware run (November 9, 2026; NRO `134b0959…`).**

- **Results:** 7 tests, all mismatched, on 14 to 53 differing pages. The game then froze in the
  next test; the logger thread kept running, so it was a hang, not a crash.
- **Timings:** capture and restore about 30 ms each, page hashing about 62 ms. Re-simulating
  7 frames took 0.3 to 7 ms, which is fast enough for rollback; full copies are not.
- **Where the differing global pages were:** the audio driver (`AXDriver_*` in `axdriver.c`,
  `HSD_Synth_*` and `hsd_SynthSFXNodes` in `synth.c`), render and text caches (`HSD_VIData`
  and `garbage` in `video.c`, `render_alloc_data` in `tev.c`, `HSD_SisLib_FontAtlas`), a
  log-once table in `eflib.c`, memory-card state (`card.c`) and debug tables (`db*.c`).
- **Cause:** melee-pc runs the game's 5 ms synth callback on the audio thread, which writes those
  globals and the audio heap in MEM1 on its own. Restoring them under it is the likely cause of
  the hang.

**Fixes in the next build (NRO `167eb73e…`):**

- Those objects build as `melee_game_host`, outside `mgdata`/`mgbss`. mgbss drops from 1,751 KB
  to 1,427 KB.
- The audio heap is left out of MEM1. A link-time `--wrap=OSCreateHeap` records every heap's
  range, and the one whose handle is `HSD_Synth_804D6018` is subtracted.
- Copies and restores run under melee-pc's interrupt lock (`OSDisableInterrupts`), which the
  audio thread holds while the synth callback runs.

The log's first `[rollback]` line now prints the regions and the audio heap range.

**Second run (NRO `167eb73e…`).** 48 tests over about 8.5 minutes and no freeze, so the audio fix
held. Memory still differed:

- **During a match:** 50 to 77 pages in one MEM1 area (about +0x6f0000 to +0x880000).
- **Out of a match:** a constant 11 pages.
- **Global pages:** what the render pass writes between frames (lights `active_lights` and
  `hsd_804D2*`, particles `psTexGroupArray`, `ptclref_*`, `particle_list` and `pvmtx`), audio
  reverb (`s_auxB`, `s_revstd_work`) and the preload cache.

A re-simulated frame is never drawn, so whole-memory equality cannot hold. Slippi judges rollback
by a gameplay checksum instead, and so does Melee Unlocked. From NRO `b5b13468…`, the patch
computes that checksum at every body end, with Melee Unlocked's formula (each fighter's
`motion_id`, `cur_pos.x`/`.y`, `dmg.x1830_percent`, `x8_spawnNum`, and each player's stocks)
plus the RNG seed (`*HSD_RandSeedPtr`).

The self-test now reports `gameplay match|MISMATCH (reference/re-simulated)` as the pass/fail.
Whole-memory equality stays in the line as a diagnostic.

**Third run (NRO `b5b13468…`): 25 of 25 gameplay matches** over a full match, re-simulating
7 frames in 0.5 to 5.6 ms each. Re-simulation from a restored snapshot is deterministic for
gameplay, so rollback is possible on this port.

What is left for rollback itself is the capture cost: about 30 ms for a full copy, while Slippi
captures nearly every frame online. Copying only what the game uses (each heap up to its
high-water mark, rather than all 96 MB of MEM1) and spreading the copy over worker cores is
written (`SnapshotSpans`, `Pool`) and is what online play uses; it has not been timed on hardware.

## Account and sign-in design

The donor reads an official Slippi `user.json` and requires a nonempty `uid`, `playKey` and
`connectCode`. Slippi offers no device-code login a console could finish on its own. The official
[`UserManager`](https://github.com/project-slippi/slippi-rust-extensions/blob/main/user/src/lib.rs)
also logs in from the local file. So the launcher moves the file instead:

- **Sign-in session**: a 6-digit code and a 10-minute limit. It stops after one valid upload or 5
  wrong codes.
- **Limits**: 8 KB of headers and 16 KB of body, and chunked uploads are refused.
- **Saving**: the account is validated before it is saved, through a temporary file. A bad upload
  never replaces a good account.

The play key is never put in a URL or QR code, never logged and never echoed in the response. The
transfer is plain HTTP on the local network; LAUNCHER.md says so.

Loading the file is not presented as server verification. `RefreshProfile` (no UI button yet) asks Slippi's public
user service whether the uid exists and what version online play needs. Only the matchmaking
server checks the play key.

## Other projects checked

- [999sian/melee-pc](https://github.com/999sian/melee-pc/tree/0fb0af3304d3469308b5574369a6ec86f145632f)
  now has its own rollback and snapshot work, much newer than our pinned `7c9a468f` base.
  - Its README requires peers to run the same build, so it does not establish Slippi Dolphin
    compatibility.
  - Its Aurora integration and AArch64 snapshot work are useful references.
  - Do not update our base wholesale; that would invalidate the Switch patch set.
- [GurekamDhillon/gd-melee-workspace](https://github.com/GurekamDhillon/gd-melee-workspace/tree/dc9dcbc5391b1f14be259fa5b58f0da22b7a9e57)
  has native rollback, room codes and Slippi replay support, for same-version Windows/Linux play.
  Replay compatibility alone does not establish matchmaking or Dolphin compatibility.
