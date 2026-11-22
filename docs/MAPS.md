# Custom maps

`python3 kaoforge.py build MAP.json --level NAME` generates a complete level from a JSON description, validates it with the scene reader,
packs it into a PAK (using the game's own `05_wielkie_drzewa.PAK` as the donor for the hero script) and registers it in `levels.ini`.
Everything the game needs is generated; nothing from the game is copied into this repository.

| key | meaning |
|---|---|
| `floor` | `tiles` (N x N tiles), `cells`, `cell` (cell size), `z` (floor height) |
| `sky`, `ambient` | RGB 0..1 |
| `sun` | `color`, `dir` (vector towards the sun) |
| `cubes` | `"default"` or `[{"name", "box": [x0,y0,z0,x1,y1,z1]}]` (z relative to the floor) |
| `barrels` | `"default"` or `[[x, y], ...]` – smashable barrels (hit them; they respawn) |
| `props` | `"all"` (whole catalog in rows), `[]`, or `[{"prop": "tree01.ar", "pos": [x,y], "rot": degrees, "scale": 1, "z": 0}]` |

## Props from the game's levels
Props are not shipped. To get them from **your own** copy: start the game, Home menu -> **Export props from ALL levels** (it loads every level and
writes `<game>/kaoforge_export/*.bin`; takes a few minutes). Then `python3 kaoforge.py catalog` lists them and maps can use them by name.

## Notes
- Every mesh gets collision (box broad-phase + triangle tree). Meshes must stay under 65,536 vertices.
- Big maps are heavy: the scene is loaded by the game at once.
- Scene format notes: `docs/ENGINE.md` (sections on `.ar`, collision and rendering).
