# KaoForge

A mod loader and map toolkit for **Kao the Kangaroo: Round 2** (2005, Steam). Mods are small C programs (DLLs) that can hook game code,
read and move scene objects, draw in 2D and 3D, add console commands and menu entries – and be reloaded without restarting the game.
KaoForge also builds **custom levels** from a JSON file (floor, cubes, barrels, and props taken from the game's own levels).

> **Status: 0.1.0, early and experimental.** Written for the Steam build on Linux/Proton. Expect rough edges; bug reports welcome.
> **You need your own copy of the game.** KaoForge contains no game files, no game assets and no DRM circumvention.
> Not affiliated with or endorsed by the game's authors or publisher; all game content belongs to its owners.

## Requirements
- Kao the Kangaroo: Round 2 from Steam – `kao2.exe` of **8,423,568 bytes** (other builds: the loader stays inactive).
- Linux with Steam/Proton (Windows should work too but is untested).
- `i686-w64-mingw32-gcc` (mingw-w64, 32-bit), Python 3.8+.

## Install
```bash
git clone <this repo> && cd kaoforge
python3 kaoforge.py doctor      # finds the game, checks everything
python3 kaoforge.py install     # builds + installs the loader and the example mods
```
Then add this to the game's Steam **launch options** (Proton needs it to load the local `glut32.dll`):
```
WINEDLLOVERRIDES="glut32=n,b" %command%
```
Start the game. `kaoforge.log` in the game folder should begin with `KaoForge 0.1.0 (API 4) loaded`.
Remove everything with `python3 kaoforge.py uninstall`.

## In the game
| key | action |
|---|---|
| **Home** | KaoForge menu (click with the mouse): camera, sliders (movement / flight speed, jump height), fly, shadows, level selector, export |
| **`** or **F10** | console with autocomplete (`help` lists commands; `reload` reloads mods) |
| F1 / F2 / F3 / F4 | fly (look-direction flight, PgUp/PgDn = vertical) / fast / high jump / god mode |
| F8 / F11 | FXAA / soft shadows |
| mouse | free orbit camera (wheel = distance), left button = punch |

## Writing a mod
```c
#include <windows.h>
#include "kaoforge.h"
static const KaoAPI *K;
static void tick(float dt) { float p[3]; if (K->hero_pos(p) && p[2] < -500) { p[2] = 100; K->hero_teleport(p); } }  // rescue from the void
__declspec(dllexport) int KaoMod_Init(const KaoAPI *api) { K = api; K->on_tick(tick); return 0; }
```
Put it in `mods/<name>/<name>.c`, run `python3 kaoforge.py mods`, then type `reload` in the game console.
See [docs/MODDING.md](docs/MODDING.md) and the examples in `mods/`.

## Custom maps
```bash
python3 kaoforge.py new maps/mine.json
python3 kaoforge.py build maps/mine.json --level 97_mine --title "Mine"
```
Then pick the level in the Home menu. Props from the game's levels need a one-time in-game export – see [docs/MAPS.md](docs/MAPS.md).

## Layout
`loader/` the proxy DLL (C) · `include/kaoforge.h` mod API · `mods/` example mods · `tools/` scene/PAK tooling (Python) ·
`maps/` example maps · `docs/` guides and engine notes (`ENGINE.md`: addresses, structures, file formats).

## License
MIT (see `LICENSE`). Contributions welcome.
