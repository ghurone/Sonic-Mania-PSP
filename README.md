# Sonic Mania PSP

A port of **Sonic Mania Plus** to the **PSP** (tested on a PSP-3000 with CFW), built on top of the
[Sonic Mania decompilation](https://github.com/RSDKModding/Sonic-Mania-Decompilation) and the
[RSDKv5 engine decompilation](https://github.com/RSDKModding/RSDKv5-Decompilation), both by RSDKModding.

> **This repository does not contain the game.** You need your own copy of Sonic Mania Plus: its `Data.rsdk` is
> converted, on your own computer, into the `Data.psp` the PSP reads. Do not share your `Data.psp` or `Data.rsdk`.

## Credits and license

- **Sonic Mania**: SEGA, Christian "Evening Star" Whitehead, Headcannon and PagodaWest Games.
- **Sonic Mania and RSDKv5 decompilations**: Rubberduckycooly, chuliRMG (RMGRich) and the other contributors at
  [RSDKModding](https://github.com/RSDKModding). The decompilation and the original code are **not mine**.
- **PSP port** (what this repository adds): ghurone.
- Libraries included in the engine: [stb_vorbis](https://github.com/nothings/stb) (Sean Barrett),
  [tinyxml2](https://github.com/leethomason/tinyxml2) (Lee Thomason), miniz and iniparser.

The code follows the original licenses: [SonicManiaDecomp/LICENSE.md](SonicManiaDecomp/LICENSE.md) (Sonic Mania) and
[SonicManiaDecomp/dependencies/RSDKv5/LICENSE.md](SonicManiaDecomp/dependencies/RSDKv5/LICENSE.md) (RSDKv5). Among
other things, they forbid commercial use and distributing the game's files.

## Repository layout

- `SonicManiaDecomp/`: the Sonic Mania decompilation with the port's changes. The engine lives inside it, in
  `dependencies/RSDKv5` (rather than as a submodule, since the port changes the engine): it is RSDKv5-Decompilation at
  commit [`bd59396`](https://github.com/RSDKModding/RSDKv5-Decompilation/commit/bd5939658fa9987bd2d4384799cacd57aa432eec).
- `psp/`: the Makefile that builds the EBOOT.
- `tools/`: the converters that turn your `Data.rsdk` into `Data.psp`.

The git history keeps the original code and the port's changes in separate commits.

## What the port does

- Drawing on the GE (the PSP's graphics chip): tile layers, the special stages' rotozoom, sprites and 3D models.
- Music in ATRAC3, decoded by the PSP's Media Engine.
- All the data in a single file (`Data.psp`), laid out to load quickly from the Memory Stick.
- Sleep and resume (power button) without freezing the game or losing the music.
- Portuguese as a language (a PORTUGUÊS button in the language menu).
- An achievements page in the main menu (the PSP has no system list of them).

## The easy way: Google Colab

[![Open In Colab](https://colab.research.google.com/assets/colab-badge.svg)](https://colab.research.google.com/github/ghurone/Sonic-Mania-PSP/blob/master/SonicManiaPSP.ipynb)

The notebook does everything below in Google's cloud, for free, with nothing to install: you upload your `Data.rsdk`
(or pick it from Google Drive), click **Runtime → Run all**, and about 20 minutes later you download
`SonicManiaPSP.zip`. Extract it to the root of your Memory Stick and you are done.

The rest of this page is for doing it on your own computer.

## Requirements

- A **PSP with 64MB of RAM and CFW**: PSP-2000, PSP-3000 or PSP Go. Only the PSP-3000 has been tested. The PSP-1000
  (32MB) does not have enough memory.
- The **`Data.rsdk` of Sonic Mania Plus** (version 1.06, the Steam one), found in the game's install folder.
- **Python 3** with the `numpy`, `pillow` and `lz4` packages:
  ```
  pip install numpy pillow lz4
  ```
- **ffmpeg** and **ffprobe** on the PATH (for the music and the videos).
- **[atracdenc](https://github.com/dcherednik/atracdenc) 0.2.3**, the ATRAC3 encoder. On Windows, put `atracdenc.exe`
  in `tools/atracdenc/` (or point to it with `--atracdenc`).
- **[pspdev](https://github.com/pspdev/pspdev)** (toolchain and pspsdk), to build the EBOOT.

## How to use

### 1. Convert the game

```
python tools/build_psp_data.py "C:\path\to\the\game\Data.rsdk" Data.psp
```

The conversion takes a few minutes. To leave out Portuguese or the achievements page, add `--no-portuguese` or
`--no-achievements`.

### 2. Build the EBOOT

With pspdev on the PATH:

```
cd psp
make -j8
```

This produces `psp/EBOOT.PBP`. The PSP menu's icon, background image and sound are optional: put your own `ICON0.PNG`
(144x80), `PIC1.PNG` (480x272) and `SND0.AT3` in the `psp/` folder before building and they go into the EBOOT.

### 3. Install on the PSP

Create the folder `PSP/GAME/SonicMania/` on the Memory Stick and copy `EBOOT.PBP` and `Data.psp` into it.

## Status

- Mania Mode, Encore Mode and Time Attack work, and the regular stages run at 60 fps.
- The special stages (UFO) run at 60 fps most of the time. UFO6 drops to ~40-50 fps where the horizon sits high on
  the screen, and the pinball bonus stage runs at ~55 fps.
- Rarely, the PSP freezes as the intro video starts (the Media Engine stops responding). If it happens, restart the
  game.
