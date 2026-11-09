# Melee NX Launcher

The launcher is a separate homebrew app, `melee-nx-launcher.nro`, that opens before the game. From
it you start Melee, turn mods on and off, sign in to Slippi and manage your game data. It then starts
`melee.nro` and closes. When you quit the game, the launcher opens again.

You don't have to use it. Starting `melee.nro` directly from the Homebrew Menu works exactly as it
did before.

- [Install](#install)
- [Using it](#using-it)
- [Mods](#mods)
- [Slippi](#slippi)
- [How the launcher starts the game](#how-the-launcher-starts-the-game)
- [Building and testing](#building-and-testing)
- [Source map](#source-map)
- [Credits and licences](#credits-and-licences)
- [Known limits](#known-limits)

## Install

Lay out the SD card like this. Everything goes under `/switch/melee-nx/`:

```text
sdmc:/switch/melee-nx/melee-nx-launcher.nro   the launcher; the Homebrew Menu lists it
sdmc:/switch/melee-nx/bin/melee.nro           the game; bin/ keeps it out of the Homebrew Menu
sdmc:/switch/melee-nx/GALE01.iso              your own disc image (NTSC-U 1.02), any file name
sdmc:/switch/melee-nx/mods/                   mods (created on first start)
sdmc:/switch/melee-nx/slippi/user.json        your Slippi account (created when you sign in)
```

The project's main build is the deko3d game (`MELEE_NX_GFX=deko3d`, see [DEKO3D.md](DEKO3D.md)),
installed as `bin/melee.nro` and started through the launcher. The launcher works with either
graphics build, because it only passes `launch.cfg`.

The launcher looks for the game at `bin/melee.nro`, then `/switch/melee-nx/melee.nro` (where
earlier releases put it), then `/switch/melee.nro`. If you leave `melee.nro` in
`/switch/melee-nx/`, the Homebrew Menu lists both apps. That works too.

Open the launcher from a game: hold **R** while starting any game, then choose the launcher.
Started from the Album, homebrew gets too little memory for Melee, and the launcher shows a
warning.

## Using it

The rail on the left lists the pages and holds the **Play** button. You can press **+** on any page
to start the game.

| Input | Action |
|---|---|
| D-pad / left stick | Move |
| A | Select, turn on or off |
| B | Back or cancel |
| L / R | Previous / next page |
| + | Play |
| X | Mods page only: move the focused mod up in priority |
| Touch | Tap anything |

**Play** shows whether the game is ready and how many mods will load. *Load mods* is the master
switch: turn it off to play vanilla without changing your mod list.

**Mods** lists every installed mod. See [Mods](#mods).

**Slippi** shows your account. See [Slippi](#slippi).

**Game Files** has these rows:

- *Disc image*: picks your disc. The launcher searches `/switch/melee-nx`, the SD root,
  `/roms/gc`, `/roms/gamecube` and `/games`. Each image is checked to be Melee NTSC-U 1.02 before
  it can be picked.
- *Extracted game files*: copies the game files out of the disc to `files/` for much faster
  loading. This uses the same extractor as the game's first-run screen. You can cancel it safely
  with B.

**Settings** has the accent colour, *Skip the launcher* and *Return here after playing*. With
*Skip the launcher* on, the launcher starts the game straight away. Hold **L** while the launcher
starts to see it instead.

Settings are stored in `launcher.json` in `/switch/melee-nx`.

## Mods

Melee NX never modifies your disc image or your extracted files. An enabled mod only changes which
file the game loads. Turn the mod off and the original comes back.

### Adding mods

Copy mods into `sdmc:/switch/melee-nx/mods/`, then press **Rescan** on the Mods page (or restart
the launcher). These forms are accepted:

| You copy | What happens |
|---|---|
| A folder of game files, laid out like the disc: `mods/Cool Fox/PlFxGr.dat` | One mod. |
| The same with the files in a `files/` subfolder, as in an extracted mod ISO: `mods/Pack/files/GrNBa.dat` | One mod; `files/` is stripped. |
| A `.zip` of either: `mods/Cool Fox.zip` | Unpacked into `mods/Cool Fox/`. The zip stays; delete it if you like. |
| A single costume `.dat`, with any name: `mods/Tom Nook.dat` | Its costume slot is read from the file itself and it becomes `mods/Tom Nook/PlFxGr.dat`. |
| A costume download as it comes from costume sites: `mods/Sponge/PlMrYe sbsp.dat`, `csp.png`, `stc.png` | The DAT is identified from its contents and replaces `PlMrYe.dat`. The PNGs are ignored. |

Any `.dat` or `.usd` inside a mod whose name is not a real game file gets identified the same way.

Costume identification follows Melee Unlocked's rule. The DAT's root table must name exactly one
existing slot as `Ply<Fighter>5K<Colour>_Share_joint`. For example, `PlyFox5KGr_Share_joint`
means Fox, green, `PlFxGr.dat`. The default costume's root has no colour, but its file ends in
`Nr`. A DAT that names no slot, names two, or has broken tables is refused, and the reason is
shown. The file name is never used as evidence.

A mod folder may contain a `mod.json`:

```json
{ "name": "Cool Fox", "author": "someone", "version": "1.0", "description": "A cooler Fox." }
```

`mod.json`, images (`*.png`, `*.jpg`), `*.txt`, `*.md`, `*.zip`, `__MACOSX/` and dotfiles inside
a mod are ignored. Every other file is a game-file replacement.

Character-select portraits (CSP) and stock icons shipped as PNGs are not applied yet. They need
texture replacement in the renderer, which Melee Unlocked does and melee-nx does not, so a reskinned
costume keeps the original portrait and stock icon.

### On, off and priority

Press **A** on a mod to turn it on or off. Enabled mods are listed first, numbered by priority. When
two enabled mods replace the same file, the one with the lower number wins. **X** raises the
focused mod one place.

The choices are saved to `mods/mods.json` straight away. A mod you add later starts off.

### What works

The game reads replacement files through melee-pc's file loader (`lbFile`). This covers anything
the game loads as an archive:

- costumes (`Pl*.dat`)
- stages (`Gr*.dat`)
- effects (`Ef*.dat`)
- menus and HUD (`Mn*`, `If*`, `GmPause.usd`)

Audio banks and movies are read another way and are not replaced.

A few files exist twice on the disc, as a `.dat` and an English `.usd`: Captain Falcon's red
costume (`PlCaRe`) and Pokémon Stadium (`GrPs`). The NTSC-U game loads the `.usd`. If a mod ships
only one of the pair, the launcher serves it for both. A file the disc does not have gets a warning
under the mod, since nothing would ever load it.

Code mods do not work: 20XX, UnclePunch Training Mode, Akaneia and m-ex fighters. They patch the
GameCube program (the DOL), and melee-nx runs the game compiled from source, so those patches have
nothing to apply to. Melee Unlocked ports some of them to C by hand. That is separate, per-mod work.

### How the game applies mods

When you press Play, the launcher writes `mods/active.txt`. Each line holds a disc path and an SD
path, separated by a tab, and the first entry for a path wins:

```text
# melee-nx mod overlay v1: disc path<TAB>SD path, first entry wins
PlFxGr.dat	sdmc:/switch/melee-nx/mods/Cool Fox/PlFxGr.dat
GrNBa.dat	sdmc:/switch/melee-nx/mods/Pack/files/GrNBa.dat
```

The game receives the file's path in `MELEE_MODS_MANIFEST`. melee-pc's `file_cache.cpp` reads the
file once, matching names case-insensitively, and serves each listed file instead of the original.
This change lives in `switch/patches/melee-switch-gcc-compat.patch`. The file's size comes from the
replacement, so a mod need not match the original's length. If a listed file is missing, the game
falls back to the original. With no mods on, `active.txt` is deleted and the game runs vanilla.

## Slippi

### Signing in

Slippi has no sign-in that a console can complete on its own. Your account is the `user.json` file
the Slippi Launcher writes when you log in on a PC. It holds your uid, a secret play key, your
connect code and display name. Melee Unlocked uses the same file. The launcher accepts it in two
ways.

**From a phone or PC (recommended).**

1. Connect the Switch to Wi-Fi.
2. On the Slippi page, choose *Sign in*. The launcher shows an address such as
   `http://192.168.1.20:8080` and a 6-digit code.
3. Open the address in a browser on the same network.
4. Pick your `user.json` (or paste it), type the code and press *Sign in*. The page says where the
   Slippi Launcher keeps the file: `%AppData%\Slippi Launcher\netplay\User\Slippi` on Windows,
   `~/.config/Slippi Launcher/netplay/User/Slippi` on Linux; on a Mac, search Finder for it.

Only one valid upload is accepted. The sign-in stops after it succeeds, after 10 minutes, or after
5 wrong codes. B cancels it. Requests are limited to 8 KB of headers and 16 KB of body, and chunked
uploads are refused. The file is checked before it is saved: it needs a uid, a play key and a
connect code. A bad upload never replaces a good saved account.

This is plain HTTP on your local network. Use it at home, not on public Wi-Fi.

**By SD card.** Copy `user.json` to `sdmc:/switch/melee-nx/slippi/user.json` and restart the
launcher.

The Slippi page shows your name and connect code, with *Switch account* and *Sign out* (deletes
`user.json` after you confirm). `RefreshProfile` in `slippi_account.cpp` can ask Slippi's public
user service for the current name, code and required version; it has no button for now and will be
used by the online port.

The play key is never displayed, logged, put in a URL or sent anywhere except the Switch itself.
The sign-in page response leaves it out too.

### Online play

Online play is not in this build. Matchmaking and rollback netcode need the game-side Slippi layer
first, and that is still being ported. Status and plan: [SLIPPI.md](SLIPPI.md).

## How the launcher starts the game

When you press Play, the launcher does the following:

1. It rechecks the game data and writes `mods/active.txt`, or deletes it when no mods are on.
2. It writes `sdmc:/switch/melee-nx/launch.cfg` through a temporary file:

   ```text
   # Written by the Melee NX launcher; read by melee.nro --launch
   disc=sdmc:/switch/melee-nx/GALE01.iso
   mods=sdmc:/switch/melee-nx/mods/active.txt
   return=sdmc:/switch/melee-nx/melee-nx-launcher.nro
   ```

   `disc` is the disc image, or `@melee-nx-loose` to boot from the extracted files. `mods` and
   `return` may be empty.
3. It queues the game with `envSetNextLoad("<melee.nro>", "<melee.nro> --launch")` and exits. The
   Homebrew loader then starts the game.

The game (`switch/src/main_switch.cpp` and `switch/src/game_data/launch_request.cpp`) reads
`launch.cfg` only when it was started with `--launch`. It then:

- Validates `disc` with the same checks as its own first-run screen: a Melee 1.02 image, or a
  finished extraction plus `disc.meta` and `disc-boot.bin`. If the check fails, it falls back to
  that first-run screen, so a stale `launch.cfg` can't stop the game from starting.
- Sets `MELEE_MODS_MANIFEST` when `mods` is set.
- Registers an `atexit` handler that queues `return` again. A normal exit, or quitting from inside
  the game with `exit()`, reopens the launcher. The handler checks that the file exists and that
  the loader supports it.

`melee-nx-boot.log` records the request in its `05 gate` line.

## Building and testing

The launcher is an SDL2 app built against devkitPro portlibs (SDL2, SDL2_ttf, SDL2_image, libcurl,
zlib). It also compiles the game's own disc extractor (`switch/src/game_data/disc_extractor.cpp`
and `switch/src/nod/gc_disc.c`), so both sides validate and extract discs the same way.

On Windows with a local devkitPro, without Docker:

```powershell
powershell -ExecutionPolicy Bypass -File launcher\build-native.ps1 -DevkitPro E:\tools\devkitPro
```

This writes `build\launcher\melee-nx-launcher.nro` and prints its SHA-256. In the SDK container,
`launcher/CMakeLists.txt` builds the same sources with devkitPro's `Switch.cmake`.

The version (0.5.0, matching the game), title and author are set in `launcher/CMakeLists.txt`. The
launcher uses the game's icon, `switch/icon.jpg`.

To run the host tests:

```powershell
powershell -ExecutionPolicy Bypass -File launcher\tests\run-host-tests.ps1
```

They build with llvm-mingw clang and cover:

- costume identification, including bad tables and conflicting roots
- mod scanning (folders, `files/`, loose DATs, `.usd` twins, warnings), priority, conflicts, saved
  order and the manifest
- `user.json` validation and saving
- the sign-in HTTP parser and handler (limits, wrong codes, play key never echoed)
- the `launch.cfg` round trip between the launcher's writer and the game's parser

Zip unpacking and the disc extractor are stubbed on the host. Sockets are left out with
`-DMNX_NO_SOCKETS`.

`python builder/verify-patches.py` checks the game-side overlay hunk together with the rest of the
patch set.

## Source map

| Path | Purpose |
|---|---|
| `launcher/source/app/` | Entry point, SDL loop and shared state (`app.cpp`), controller and touch input |
| `launcher/source/ui/` | Drawing (`canvas`), widgets, focus navigation, overlays, theme |
| `launcher/source/screens/home*.cpp` | The rail and the Play, Mods, Slippi, Game Files and Settings pages |
| `launcher/source/screens/flows.cpp` | Progress screen, disc picker, Slippi sign-in screen |
| `launcher/source/data/paths.h` | Every SD path the launcher and the game share |
| `launcher/source/data/mods.*` | Mod catalog, priority and manifest |
| `launcher/source/data/dat_identity.*` | Costume slot from DAT roots |
| `launcher/source/data/slippi_account.*` | `user.json` validation, save, delete and profile refresh |
| `launcher/source/data/lan_signin.*` | Local sign-in web page and server |
| `launcher/source/data/game_data.*` | Disc search, validation, extraction job |
| `launcher/source/data/launch.*` | `launch.cfg` and `envSetNextLoad` |
| `launcher/tests/` | Host tests and stubs |
| `switch/src/game_data/launch_request.*` | Game side of the handoff |
| `ref/melee-pc/src/pc/file_cache.cpp` (patched) | Mod overlay in the game's file loader |

Rule for UI code: a widget callback never rebuilds the widget list it belongs to. It queues the
rebuild with `HomeScreen::Later`, which runs at the start of the next frame.

## Credits and licences

The launcher's own code is GPL-3.0-or-later, like the rest of melee-nx.

- **Melee Unlocked** ([Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked),
  GPL-2.0-or-later; inspected at `6d5e2e4b`) is the design base. The mod catalog model (vanilla
  files untouched, per-file override, priority, `.usd` twins), the costume fighter table and root
  rule (`dat_identity.cpp`), the `user.json` account rules and the Slippi user-service endpoint all
  follow its `cosmetic_mods.cpp`, `slippi_account.h` and `slippi_net.cpp`. GPL-2.0-or-later code
  may be used under GPL-3.0.
- **KartPadNX** ([KawaiiBunga/KartPadNX](https://github.com/KawaiiBunga/KartPadNX)): the SDL UI
  toolkit under `ui/`, the screen stack, the input reader, `fsutil.h` and `zip_extract.cpp` started
  from its launcher. The look, layout and pages here are Melee NX's own.
- **nlohmann/json** 3.11.3, MIT (`launcher/include/nlohmann`).
- **Nunito**, SIL OFL 1.1, and **Material Icons Round**, Apache 2.0
  (`launcher/romfs/fonts/SOURCES.md`).

## Known limits

- **Not run on a console yet.** The NRO builds, the host tests pass and the changed game files
  compile, but these have not been exercised on hardware:
  - the launcher UI
  - starting the game through `envSetNextLoad`
  - returning to the launcher
  - the sign-in server
  - HTTPS through devkitPro's curl

  The UI has not been rendered on a PC either: the host preview needs SDL2 for Windows, which this
  machine doesn't have.
- No QR code on the sign-in screen; type the address.
- Mods cannot replace audio banks or movies, and code mods are not supported.
- Zips are unpacked with the KartPadNX extractor: stored or deflated entries, no Zip64, no
  encryption.
