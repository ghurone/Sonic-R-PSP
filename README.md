# Sonic R PSP

A port of **Sonic R** (the 1998 PC version) to the **PSP**, built on top of jnmartin84's
[Sonic R decompilation](https://github.com/jnmartin84/sonic-r).

> **This repository does not contain the game.** You need your own copy of the Sonic R PC CD: its files and its
> music are converted, on your own computer, into the folder the PSP reads. Do not share that folder or the
> `SONICR.PAK` inside it.

## Credits and license

- **Sonic R**: Traveller's Tales and Sonic Team, published by SEGA.
- **Sonic R decompilation**: [jnmartin84](https://github.com/jnmartin84/sonic-r). The game code in `src/sdl` is
  based on it, and the decompilation itself is **not mine**; `src/sdl` is no longer identical to it, since the port
  changed it (see [src/sdl/ORIGIN.md](src/sdl/ORIGIN.md)).
- **Egg Robo texture fix** (applied to your data when the Memory Stick folder is made): InvisibleUp, from
  CheatFreak47's [SRUpdater](https://github.com/cheatfreak47/SRUpdater).
- **PSP port** (what this repository adds): ghurone.

The code follows the decompilation's license, [src/sdl/LICENSE](src/sdl/LICENSE): it may be downloaded, built and
changed, but **never sold**, in any form, with or without the game's data.

## What the port does

- **60 fps**: the game runs its logic at 30 Hz, as on PC, and the PSP draws an extra frame halfway between two
  ticks (cameras, characters, poses, decorations and particles interpolated). 30 fps is still an option.
- Drawing on the GE (the PSP's graphics chip): the track, the ground, the characters (skinned on the GE) and the fog.
- Widescreen (16:9) in one-player races, with the HUD at the screen's edges, or 4:3 as on PC.
- A longer draw distance than the PC's (up to 2x "Very Far"). This, the FPS and the screen shape are set in
  **Options → Graphics**.
- Music in ATRAC3, decoded by the PSP's Media Engine and streamed from the Memory Stick.
- All the game's data in a single file (`SONICR.PAK`): a race's files load in about 1.5 s.
- Fixes for bugs the decompilation had, checked against the original `SONICR.EXE`: the menus remember the chosen
  character and course, Radiant Emerald's start sign is back over the track, the robots' exhaust flame flickers again.
- Faster menu cursors.

## Download

The [releases](https://github.com/ghurone/Sonic-R-PSP/releases) have a ready-made `EBOOT.PBP`. It is only the game's
program: the game itself still comes from your own CD. Follow the steps below but skip step 3 (and pspdev), and give
step 4 the EBOOT you downloaded:

```
python tools/make_ms_folder.py --eboot "C:\path\to\EBOOT.PBP"
```

## Requirements

- A **PSP with custom firmware** (CFW), to run homebrew. The port is made to fit in a PSP-1000's 32MB too, but that
  was only checked by simulating its memory on a later PSP, not yet on a real PSP-1000.
- The **Sonic R PC CD**, as a `.bin`/`.cue` dump (tested with the USA disc) or the disc itself.
- **Python 3** (no extra packages).
- **[atracdenc](https://github.com/dcherednik/atracdenc) 0.2.3**, the ATRAC3 encoder, for the music. Put
  `atracdenc.exe` in `tools/atracdenc/`, have it on the PATH, or point to it with `--atracdenc`.
- **[pspdev](https://github.com/pspdev/pspdev)** (toolchain and pspsdk), only to build the EBOOT yourself, plus `make` and a POSIX
  shell (`find`, `cp`, `mkdir`): Linux, macOS, WSL, or on Windows the native pspdev with MSYS2's tools on the PATH.

## How to build

Every command runs from the repository's root.

### 1. Extract the CD

From a `.bin`/`.cue` dump (one `.bin` per track, as Redump's, or a single `.bin`):

```
python tools/extract_cd.py "C:\path\to\Sonic R (USA).cue"
```

This writes the disc's files to `sonic-r-cd-rip/` and its audio tracks (the music) to
`sonic-r-cd-rip/MUSIC/track2.wav` ... `track21.wav`.

From the disc itself: copy all of its files into `sonic-r-cd-rip/`, then rip audio tracks 2 to 21 with any CD ripper
as 44.1kHz 16-bit stereo WAV, named `track2.wav` ... `track21.wav`, into `sonic-r-cd-rip/MUSIC/`.

### 2. Convert the music

```
python tools/convert_music.py
```

This converts the 20 WAVs to ATRAC3 in `build/MUSIC/`. Tracks already converted are skipped, so this runs once.

### 3. Build the EBOOT

With the pspdev toolchain on the PATH:

```
make -j8
```

This builds `build/EBOOT.PBP`. To give the game an icon, a background or a sound in the PSP's menu, put them in
`assets/` before running `make` (see [assets/README.md](assets/README.md)).

### 4. Assemble the Memory Stick folder

```
python tools/make_ms_folder.py
```

This builds `dist/PSP/GAME/SonicR/` with the EBOOT, `SONICR.PAK` (the game's data, with the Egg Robo fix and the
announcer's replay lines cut from your CD's sound files), `MUSIC/`, and the empty `SAVE/` and `GHOST/` folders where
the game keeps your saves and time-attack ghosts.

### 5. Copy it to the PSP

1. Connect the PSP to the computer..
2. Copy the `SonicR` folder from `dist/PSP/GAME/` into the `PSP/GAME/` folder on that drive.

To update the game, run steps 3 and 4 again and copy the folder the same way. If you've played already, don't
replace the `SAVE` and `GHOST` folders on the PSP: they hold your saves and time-attack ghosts.

## In game

- **Select + L + R** takes a screenshot (`shot000.bmp`, `shot001.bmp`, ... next to the EBOOT).
- **Options → Graphics**: FPS (30 or 60), Draw Distance (1x, 1.5x or 2x "Very Far") and Screen (4:3 or 16:9).

## Repository layout

- `src/sdl/`: the game code. It started as a copy of the decompilation's SDL build (`source/sdl/src`, as of
  2026-09-18) and has since been changed by the port: PSP hooks, 60 fps, faster loads and menus, and bug fixes.
  [src/sdl/ORIGIN.md](src/sdl/ORIGIN.md) lists the changes.
- `src/psp/`: the PSP's own renderer (GE), platform layer, sound, 60 fps interpolation and options page. It replaces
  the desktop renderer, platform and sound in `src/sdl`, the same way the decompilation does for its Dreamcast build.
- `tools/`: `extract_cd.py`, `convert_music.py` and `make_ms_folder.py` (steps 1, 2 and 4).
- `assets/`: where your own icon, background and sound for the PSP's menu go (none included).

## Development builds

`make` takes a few switches. Run `rm -rf build/obj` when switching between a debug and a release build (not
`rm -rf build`: the converted music is in `build/MUSIC`).

| Switch | What it does |
| --- | --- |
| `DEBUG_LOG=1` | The game's debug log, written to `log.txt` next to the EBOOT. |
| `BUILD_PRX=1` | Builds `build/SonicR.prx` to run under [PSPLink](https://github.com/pspdev/psplinkusb). |
| `DEMO=n` | With `DEBUG_LOG=1`: the title screen's attract mode always plays demo `n` (0 Island ... 8 Radiant Emerald), to test a track without unlocking it. |
| `UNLOCK=1` | Every character (but Super Sonic) and Radiant Emerald selectable, in memory only. |
| `SIM24=1` | Caps the heap at what a PSP-1000 leaves, to test its memory on a later PSP. |
| `PROF=1` | Section timers in the log. |
| `GPROF=1` | A sampled profile (`gmon.out`), read with `psp-gprof`. |

The Makefile's comments describe each one in more detail.
