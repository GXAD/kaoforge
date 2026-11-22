# KaoForge

**A mod loader and map toolkit for *Kao the Kangaroo: Round 2*** (2005, Steam).

Write mods in C that hook the game's code, move and hide objects in the scene, draw in 2D and 3D, add console commands and menu entries —
and reload them **without restarting the game**. Build your own **custom levels** from a small JSON file.

> **Status: 0.1.0 — early and experimental.** Developed and tested on Linux with Steam/Proton. Bug reports and pull requests are welcome.
> **You need your own copy of the game.** KaoForge contains no game files, no game assets and no DRM circumvention.
> It is an unofficial fan project, not affiliated with or endorsed by the game's authors or publisher.

---

## Contents
[Features](#features) · [Requirements](#requirements) · [Install](#install) · [In the game](#in-the-game) · [Console](#the-console) ·
[Writing a mod](#writing-a-mod) · [Custom maps](#custom-maps) · [How it works](#how-it-works) · [Troubleshooting](#troubleshooting) ·
[Roadmap](#roadmap) · [Contributing](#contributing) · [Legal](#legal)

## Features

**Mod loader**
- Mods are DLLs in `<game>/mods/`, built from plain C with one header ([`include/kaoforge.h`](include/kaoforge.h)).
- **Hooks:** inline detours of any game function (with a trampoline to the original), vtable / import-table / call-site hooks, byte patches. All of it is undone on reload.
- **Call game code** from your mod (cdecl / stdcall / thiscall, float results).
- **Scene access:** walk the scene tree, find nodes by name, read positions, hide objects, switch collision, teleport the hero, RTTI class checks.
- **Drawing:** 2D overlay (pixels) and 3D drawing in world space with the scene's depth buffer, text, world-to-screen projection, camera position.
- **Events:** per-frame tick, level loaded, key messages, input blocking.
- **In-game console** with autocomplete and per-mod commands, a **config file** per mod, a **menu** you can extend.
- **Hot-reload:** edit a mod, `python3 kaoforge.py mods`, type `reload` in the console.
- **Crash reports:** an exception in game code logs the faulting address, registers and a return-address trace to `kaoforge.log`.
- Safe by default: on an unknown `kao2.exe` build the loader stays inactive instead of patching the wrong addresses.

**Built-in extras** (part of the loader for now; see the [roadmap](#roadmap))
- Mouse look with a free orbit camera (wheel = distance), punch on the left mouse button.
- Fly mode that follows where you look, sliders for movement speed, flight speed and jump height, god mode.
- Soft shadows, FXAA, a level selector, smashable barrels, and an exporter for the game's own props.

**Map toolkit**
- Describe a level in JSON (floor, sky, sun, cubes, barrels, props) and build it into a real level with working collision.
- Optionally use props exported from the game's own levels (from *your* copy of the game).

## Requirements
- *Kao the Kangaroo: Round 2* from Steam — `kao2.exe` of **8,423,568 bytes** (the tested build).
- Linux with Steam/Proton. Windows should work (no Linux shell needed, see the build guide) but is untested.
- `i686-w64-mingw32-gcc` (mingw-w64, **32-bit** target) and Python 3.8+. KaoForge is distributed as source only — see **[docs/BUILDING.md](docs/BUILDING.md)** for exact install and build steps per system, including Windows with a portable compiler and a manual build without the script.

## Install
```bash
git clone https://github.com/GXAD/kaoforge && cd kaoforge
python3 kaoforge.py doctor      # finds the game, checks the exe, compiler and installed pieces
python3 kaoforge.py install     # compiles and installs the loader and the example mods (needs the compiler, see docs/BUILDING.md)
```
**Linux / Proton only:** add this to the game's **Steam launch options** (Steam → game → Properties → Launch Options), so that Proton loads
the `glut32.dll` from the game folder instead of its built-in one:
```
WINEDLLOVERRIDES="glut32=n,b" %command%
```
**On Windows you do not need this** — the game loads the `glut32.dll` placed next to `kao2.exe` by itself.

Start the game. The first line of `kaoforge.log` (in the game folder) should read `KaoForge 0.1.0 (API 4) loaded`.
If the game is not found, pass `--game /path/to/Kao the Kangaroo Round 2` or set `KAOFORGE_GAME`.
Remove everything with `python3 kaoforge.py uninstall` (restores the original `glut32.dll`).

## In the game
| Key | Action |
|---|---|
| **Home** | KaoForge menu (click with the mouse): camera, sliders, fly, shadows, level selector, export |
| **`** or **F10** | the console |
| F1 / F2 / F3 / F4 | fly (PgUp/PgDn = vertical) / fast movement / high jump / god mode |
| F8 / F11 | FXAA / soft shadows |
| Mouse | free orbit camera; wheel = camera distance; left button = punch |

Settings are saved in `kaoforge.ini`, mod settings in `kaoforge_mods.ini` (both in the game folder).

## The console
Press `` ` `` (or F10). **Tab** completes, **↑/↓** browse history, a hint line shows the usage of the command you are typing. A file named `autoexec.cfg` in the game folder runs at startup.

| Command | What it does |
|---|---|
| `help` | list all commands (including the ones added by mods) |
| `reload` | reload all mods from `mods/` |
| `levels`, `load <level>` | list / load a level |
| `pos`, `tp x y z`, `tp <node>` | print position / teleport |
| `fly`, `fast`, `jump`, `god`, `freecam` `[0/1]` | toggles |
| `speed`, `flyspeed`, `jumph`, `camdist` `[value]` | movement speed, flight speed, jump height, camera distance |
| `find <text>`, `tree [depth]` | explore the scene graph |
| `hide / show / nocol / col <node>` | hide a node, switch its collision |
| `peek <addr> [n]`, `poke <addr> <bytes>` | read / write game memory (`g` suffix = game address) |
| `exec <file>` | run a file of commands |

## Writing a mod
```c
#include <windows.h>
#include "kaoforge.h"

static const KaoAPI *K;

static void tick(float dt) {                       // runs every frame
    float p[3];
    if (K->hero_pos(p) && p[2] < -500) {           // fell into the void?
        p[2] = 100;
        K->hero_teleport(p);                       // ...bring Kao back
    }
}

__declspec(dllexport) int KaoMod_Init(const KaoAPI *api) {
    K = api;
    K->on_tick(tick);
    return 0;
}
```
Save it as `mods/rescue/rescue.c`, run `python3 kaoforge.py mods`, then type `reload` in the console. That's the whole workflow.
Full API in [`docs/API.md`](docs/API.md), a walkthrough in [`docs/MODDING.md`](docs/MODDING.md), and three examples in [`mods/`](mods):

| Example | Shows |
|---|---|
| [`hello`](mods/hello/hello.c) | the minimum: tick, 2D text, a menu entry |
| [`demo3`](mods/demo3/demo3.c) | console commands, config, labels over scene objects, a game-function hook |
| [`showcase`](mods/showcase/showcase.c) | engine telemetry through a hook, an object inspector with a 3D wireframe, removing objects live |

## Custom maps
```bash
python3 kaoforge.py new maps/mine.json
python3 kaoforge.py build maps/mine.json --level 97_mine --title "Mine"
```
Then pick the level in the Home menu (`Level: < … >` → `Load`). A map is a JSON file:
```json
{
  "floor":   { "tiles": 1, "cells": 100, "cell": 200, "z": -200 },
  "sky":     [0.45, 0.62, 0.85],
  "sun":     { "color": [0.8, 0.74, 0.62], "dir": [500, 350, 900] },
  "cubes":   [ { "name": "block01", "box": [600, -300, 0, 900, 300, 150] } ],
  "barrels": [ [500, 0], [500, 300] ],
  "props":   []
}
```
Every mesh gets collision. Props from the game's own levels need a one-time, in-game export of *your* copy — see [`docs/MAPS.md`](docs/MAPS.md). `maps/grid.json` is the default test map.

## How it works
```
 kao2.exe ──imports──▶ glut32.dll  (KaoForge proxy)  ──forwards everything else──▶ glut32_real.dll (the original)
                           │  hooks glutInit (patch the game once it has decrypted itself) and glutSwapBuffers (one tick per frame)
                           ▼
              loader: input, camera, menu, console, hook engine, scene/hero helpers
                           │  KaoAPI (a struct of function pointers)
                           ▼
              mods/*.dll — your code, loaded at startup and on `reload`
```
- The loader is a drop-in replacement for the game's `glut32.dll`. It runs on the game thread once per frame, so mods never race with the game.
- Hooks use a tiny x86 length decoder to relocate the first instructions of a function into a trampoline.
- Custom levels are real `.ar` scene archives written from scratch and packed into a PAK next to the game's own levels.
- The engine internals discovered along the way are written down in [`docs/ENGINE.md`](docs/ENGINE.md): memory layout, rendering and collision, file formats, key functions.

## Troubleshooting
| Problem | What to check |
|---|---|
| Nothing changes in the game | On Linux/Proton: is the launch option `WINEDLLOVERRIDES="glut32=n,b" %command%` set? (Not needed on Windows.) Does `kaoforge.log` exist in the game folder? |
| Log says `UNSUPPORTED BUILD` | Your `kao2.exe` differs from the tested one; the loader refuses to patch it. Please open an issue with the exe size. |
| A mod is not loaded | Look for `forge: <mod>.dll - …` lines in `kaoforge.log` (missing `KaoMod_Init`, load error). |
| The game crashes | The crash handler writes `CRASH … eip=…` and a stack trace to `kaoforge.log`. Attach it to an issue. |
| Console does not open | Press F10 (the backtick key position differs between keyboard layouts). |
| Replaced a DLL while the game was running | Close the game first; `kaoforge.py` replaces files via rename, which is safe, but loader changes need a restart anyway. |

## Roadmap
- Split the built-in extras (fly, camera, sliders, shadows) into regular mods, leaving a small core loader.
- A Windows test pass (prebuilt releases are deliberately not planned for now: KaoForge is a developer-oriented project).
- More engine coverage in the scene reader, so more of the game's props and scripts can be used in custom maps.
- Support for more `kao2.exe` builds.

## Contributing
Issues and pull requests are welcome — especially: crash logs from other setups, new example mods, notes on engine internals for `docs/ENGINE.md`.
Keep game files and game-derived data (models, textures, extracted scripts) out of the repository.

## Legal
MIT licensed (see [`LICENSE`](LICENSE)). *Kao the Kangaroo* and all game content belong to their respective owners.
KaoForge is an independent, unofficial project that works with a legitimate copy of the game and does not distribute any of its content.
