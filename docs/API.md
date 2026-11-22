# KaoForge API (v4)

The API is the struct `KaoAPI` in [`include/kaoforge.h`](../include/kaoforge.h) (documented inline). A mod exports `int KaoMod_Init(const KaoAPI *api)` (0 = OK) and optionally `void KaoMod_Shutdown(void)`.

| area | functions |
|---|---|
| hooks | `hook` (inline detour, `*orig` = trampoline), `hook_vtable`, `hook_import`, `hook_call`, `patch` (undone on reload), `write` |
| calling game code | `call(fn, self, argc, argv)` (cdecl/stdcall/thiscall), `call_f` (float result) |
| scene | `scene`, `walk`, `children`, `find_node`, `node_name`, `class_name`, `is_a` (RTTI), `node_pos`, `node_set_pos`, `node_set_visible`, `node_set_collision` |
| hero | `hero`, `hero_pos`, `hero_teleport`, `punch` |
| events | `on_tick`, `on_level`, `on_key`, `input_block` |
| drawing | `on_draw` (2D pixels), `on_draw3d` (world space: PROJECTION = P, MODELVIEW = V), `world_to_screen`, `camera`, `rect`, `text` |
| UI | `menu_item`, `command`, `print`, `exec` |
| config | `cfg_int/float/str/set` (`kaoforge_mods.ini`), `game_dir`, `log`, `va` (game address -> process address), `readable` |

All callbacks run on the game thread, once per frame. Game addresses refer to the supported build; always pass them through `api->va()` (the exe is relocatable).
