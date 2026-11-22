# Writing mods

1. Create `mods/<name>/<name>.c` (any number of `.c` files in the folder are compiled into `<name>.dll`).
2. `#include "kaoforge.h"`, export `KaoMod_Init`, register callbacks (see [API.md](API.md), `mods/hello`, `mods/demo3`).
3. (Compiler setup: [BUILDING.md](BUILDING.md).) `python3 kaoforge.py mods` builds and copies the DLLs to `<game>/mods/`.
4. In a running game open the console (`` ` `` or F10) and type `reload`. Hooks and patches installed by the mod are undone, the DLL is freed and loaded again.
   Changes to the loader itself (`loader/`) need a game restart.
5. `kaoforge.log` in the game folder shows load messages and, after a crash in game code, the faulting address and registers.

## Tips
- **Drawing in 3D** (`on_draw3d`): coordinates are world coordinates; the depth buffer of the scene is active. In `on_draw` you get a pixel-space ortho projection (origin top-left).
- **Hooks**: `hook()` copies at least 5 bytes of the function prologue into a trampoline, so the prologue must not contain conditional jumps. `orig` calls the original.
- **Scene**: nodes are engine objects with RTTI; `is_a(node, "eTriMesh")` etc. Visibility = flag `0x10000000` at `+0x1c`, collision mask at `+0x30` (see `docs/ENGINE.md`).
- **Config**: `cfg_*` read/write `kaoforge_mods.ini`; use your mod name as the section.
- Keep per-frame work small; scene walks over thousands of nodes should be throttled (see `mods/showcase`).
