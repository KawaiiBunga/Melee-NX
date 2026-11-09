# Slippi on melee-nx

Updated November 8, 2026. **Online play does not work yet.**

So far, the account side is done in the [launcher](LAUNCHER.md#slippi) and the network protocol
layer is ported. What's left is the game-side layer that turns those into a match: Slippi's online
menus, the game command bridge and rollback. Nothing in this document claims compatibility with
Slippi Dolphin until a match against it has been played.

## Status

| Piece | State | Where |
|---|---|---|
| Account (`user.json`): validate, store, sign out | Done, host-tested | `launcher/source/data/slippi_account.*` |
| Sign-in from a phone or PC on the local network | Done, host-tested (parser and handler) | `launcher/source/data/lan_signin.*` |
| Profile refresh from Slippi's user service | Done, offline-tested | `slippi_account.cpp` |
| Wire formats: packets, pad and ack messages, selections, chat ids | Ported, host-tested | `switch/slippi/slippi_protocol.*` |
| Matchmaking messages: create-ticket request, get-ticket-resp parser | Ported, host-tested | same |
| ENet for libnx | Compiles and links against libnx | `switch/slippi/third_party/enet` |
| Matchmaking client (ENet session with `mm.slippi.gg`) | Not yet | donor `slippi_net.cpp` `Matchmaking` |
| Netplay client (peer connection, pad exchange, time sync) | Not yet | donor `slippi_net.cpp` `NetplayClient` |
| Game command bridge (the EXI device the game talks to) | Not yet | donor `exi_slippi.cpp`, `native_slippi_bridge.h` |
| Online menus (CSS, stage select, chat, name tags) | Not yet | donor `sourceport/game/shim/mu_slippi_*.c` |
| Rollback (snapshot, restore, resimulate) | Not yet | donor `shim/mu_online*.c`, `native_savestate.*` |
| Replays (`.slp`) | Not yet | donor `shim/mu_replay*.c`, `slippi_playback*` |

None of this has run on a console. The launcher's sign-in and HTTPS refresh are the first parts to
check on hardware.

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

This runs 40 checks: the packet layout, pad round trip and refusal cases, selections, create-ticket
and hostile tickets. The ENet and protocol objects were also compiled with devkitA64 and linked into
a test ELF against libnx. `switch/slippi` is not part of the game or launcher build yet; it joins
the game build with the netplay client.

## Remaining work, in order

Each step keeps a playable NRO. Online modes stay hidden until steps 1 to 4 are working on
hardware.

1. **Clients on libnx.** Port `Matchmaking` and `NetplayClient` from `slippi_net.cpp` on top of
   `slippi_protocol`:
   - Replace `host::log` with the melee-nx log.
   - Replace `report::http_get` with the launcher's curl helper; it moves to a shared spot.
   - Drop YouGame, early inputs and test hooks.
   - Keep the threads, the lock order described in the donor's header, cancellation and the
     "never queue in automated runs" switch.
   - `local_address_string` needs `enet_socket_get_address`, which libnx supports.
   - Check direct peer-to-peer between two Switches on a LAN through the donor's local-peer
     tickets before going near the matchmaking server.
2. **Game command bridge.** The game sends Slippi commands through an EXI device; the donor's
   `exi_slippi.cpp` and `native_slippi_bridge.h` handle them. In melee-pc the natural seam is a
   small C API called from the ported menu code. Emulating EXI DMA is not needed, because nothing
   runs the original Gecko code.
3. **Snapshots for rollback.** This is the hard part.
   - Identify everything that simulation state lives in:
     - the MEM1 window (the game heaps, through `DP()` slots)
     - the `.data` and `.bss` of the game's own objects in `melee.elf`, separated by linker
       sections from Aurora, Dawn, SDL and libnx state
     - ARAM (`aurora_aram_base`)
   - Exclude render, audio-device, file-cache and thread state. The donor's
     `native_savestate.*` (incremental region snapshots with an exclusion list,
     `melee_game.snapexcl`) and `mu_online.c` are the model. The newer
     [999sian/melee-pc](https://github.com/999sian/melee-pc) snapshot work is a second reference
     for an Aurora build.
   - Gate: capture, advance N frames, restore, replay the same inputs and compare state bytes over
     many scenes. Check what one snapshot of up to 7 rollback frames costs on the Switch's A57 at
     60 FPS before going further.
4. **Online menus and match flow.** Port `mu_slippi_menu.c`, `mu_slippi_css.c`, `mu_slippi_sss.c`,
   `mu_slippi_chat.c` and `mu_online_rules.c` against melee-pc's scene code. They come from the
   same decomp, but symbol names and struct layouts in melee-pc's fork must be checked one by one.
   Wire in the account at `sdmc:/switch/melee-nx/slippi/user.json`; the launcher already writes
   it.
5. **Replays.** Port `.slp` recording (`mu_replay.c`) into `sdmc:/switch/melee-nx/replays/`.
6. **Validation.**
   - Play a controlled Direct match from a Switch against stock Slippi Dolphin. Check rollback,
     disconnect and reconnect, and a repeat launch.
   - After that, try Unranked.
   - Record the NRO and config hashes. Compare the downloaded deployed NRO before attributing any
     hardware result. Two melee-nx instances against each other are not evidence of Dolphin
     compatibility.

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
