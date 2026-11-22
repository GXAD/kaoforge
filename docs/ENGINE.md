# Engine notes (Kao the Kangaroo: Round 2, Steam build)

Reverse-engineering notes collected while building KaoForge. Addresses are for the supported build (`kao2.exe`, 8,423,568 bytes,
PE32 x86, preferred image base `0x400000`; the exe is relocatable, so always go through `api->va()`). Items marked **[?]** are unconfirmed.
The exe is protected by SteamStub: `.text` is decrypted in memory at startup, so code patches must happen after `glutInit` is called.

## Overview
- Custom C++ engine ("e*" classes, RTTI, ref-counted objects: `obj[1]` = refcount). OpenGL 1.x fixed-function renderer, GLUT window (`glut32.dll`), DirectInput 8.
- Scripting: text `.def` files (state machines, gadgets) loaded from the PAKs. Levels are `.ar` scene archives inside `media/<level>.PAK`.
- KaoForge works as a proxy for `glut32.dll`: `glutInit` (patches) and `glutSwapBuffers` (per-frame tick) are hooked, everything else is forwarded to the original.

## Globals and objects
| item | address / offset |
|---|---|
| application singleton pointer | `0xB3D868` (`app`); scene = `app+0xC`; hero actor = `app+0x38C`; window HWND = `0xB3BA68` |
| hero position | local `hero+0x58` (x,y,z; z is up), derived world copy `+0x78` (and `+0x98`). Actor transform at `+0x48`: quaternion x,y,z,w then position |
| level table | count `0xB34C98`, array `0xB34CA0` (pointers to level descriptors; name via levels.ini) |
| load a level | thiscall on `app+0x3F4` (object with a vtable), `vtable[0](idx+1)`; the byte `app+0x384` is zeroed first. Same path as the `loadLevel` script message (FUN_0041c9a0) |
| god mode flag | byte at `0xB34DEB` (UNLIMITED_ENERGY) |
| intro screens | `FUN_00416100` (patched to return immediately) |

### Nodes (scene graph)
- RTTI: `vtable[-1]` -> COL -> TypeDescriptor (name at `+8`); the Class Hierarchy Descriptor (COL+0x10) lists base classes (`is_a`).
- eNode: name = string object at `+0x14` (text at string+0xC); `+0x18` collision entry (eALBox) or NULL; `+0x1C` flags
  (`0x10000000` = "descend / draw this branch", `0x80`, `0x10`, `0x04` get cleared/set by the loader); `+0x20` bounding sphere (x,y,z,r); `+0x30` u16 collision mask
  (`0x10` NO_CAMERA, `0x20` NO_HERO, `0x40` NO_COLLIDE, `0x80` NO_OTHERS).
- eGroup: child count `+0x3C`, child array `+0x44`. eTransform: world quaternion `+0x68`, world position `+0x78`, scale `+0x84`. eProxy: referenced file path at `+0xAC`.
- eTriMesh: material `+0x3C`, bounding box `+0x40` (min3, max3), geoset `+0x58`.
- eGeoSet: vertex count `+0xC`; arrays (each an object: data pointer `+8`, count `+0xC`): strips `+0x14` (u16), indices `+0x18` (u16), positions `+0x1C` (16 B/vertex),
  normals `+0x24`, UVs `+0x2C`, colours `+0x3C`; collision AABB tree `+0x5C`.
- eMaterial: texture array `+0x10`, flags byte `+0x14` (bit 1 = two-sided, bits 2-4 = blend mode, bit 8 = alpha test with threshold `+0x2C`, bit 0x10 = untextured); eTexture -> eBitmap `+0x10`.
  eBitmap: width `+8`, height `+0xC`, pixels `+0x1C`, palette `+0x20`, format `+0x24`, path `+0x28`.
- Runtime additions to every scene: `&SceneAmbSndEmtr`, `$ShadowManager`, `$hero` (actor with its own sub-environment), `eAddFX_ROOT_GROUP_NAME`, `waterCirclesGroup`.

## Rendering
- Frame: `FUN_00570980` -> `FUN_005B42C0` (scene render). `FUN_005746E0` sets `GL_PROJECTION = P` (gluPerspective) and **`GL_MODELVIEW` = identity**; the camera view matrix `V`
  is built by `FUN_005AB370` (lookAt) into the game context's *own* matrix stack (`ctx+0x28480`, top pointer at `+0x1400`), not into OpenGL. Per object the game loads `V*M` into `GL_MODELVIEW` (`FUN_005741A0`).
  After the scene (`glLoadMatrixf` at `0x5B4333`) `GL_MODELVIEW` holds `V` - this is where `on_draw3d` runs.
- Camera object (eCamera): eye `+0x58`, view vector `+0x170` (target = eye + vector), up `+0x17C`.
- Lights: eDirectionalLight colour `+0x3C`, direction `+0x6C` (used as GL_POSITION with w=0, i.e. a vector *towards* the light); eAmbientLight colour in the ambient slot.
- Shadows: stencil shadow volumes (eShadowVolume). `FUN_0047F490` draws a full-screen black quad (alpha 0.5) through `glStencilFunc(GL_NOTEQUAL, 0)`. Only actors with an eShadowCaster cast shadows; static geometry does not.
- A custom mesh is only drawn if: it has a real bounding box (an empty box is culled), flags `0x1002208D`, it sits under a group named `#VS-*/1` with flags `0x7000249D` directly below `global_env`
  (the renderer only descends into branches with `0x10000000`), and the scene info string contains `.COMPILED.` (otherwise the game tries to compile the scene and needs a developer-only file).

## Collision
- Broad phase: one **eALBox** per collidable node (sweep-and-prune). Per axis, interval ends are kept sorted in linked lists (link encoding: `+-(index+2)`, sentinels `-1/+1`).
  Each box also stores a *pair table*: for `j < i`, nibble `j>>1` with bits 2/4/6 = "x/y/z intervals overlap", shifted by `(j & 1)` - length `(i+1)//2` bytes. It must not be empty (the game reads it on pair events).
- The scene embeds an eCollisionMgr: `[N]` and `N-1`; the ALBox table has `N` entries.
- Narrow phase: the geoset AABB tree (`+0x5C`): preorder nodes `min-1 (3f), left (u32), max+1 (3f), right (u32)`; a child with bit `0x80000000` is a leaf (triangle index in strip order, `0x40000000` marks odd triangles).
- Node kinds eObserver / Actor / eGroundTest / eNPCMap become *dynamic* boxes (`+0x8C = 1`); everything else is static.

## Physics and animation
- `ePhysics::update` (`FUN_004CB9A0`, thiscall `this`=physics object, `this[2]`=actor, `*(float*)(ctx+0x1C)`=dt): copies the actor transform (`actor+0x48`, 8 dwords) into its state, runs the movement
  class (`vtable+0x60`), writes the new transform back. Walking physics: previous position `+0x6E4`, new `+0x704`, velocity in the "history" object `this+0x714` (`+0x128..0x130`).
  Writes to `hero+0x78` are overwritten every frame; write `hero+0x58`, or skip this function (KaoForge's fly mode does).
- Animation: each actor embeds an eAnimate rate object at `actor+0xAC`. `eAnimate::update(dt)` (`FUN_005A4C20`): `time += dt * getRate()`; `getRate` = `FUN_005A54C0` (interpolates `+0x24 -> +0x28` over `+0x30`); `setRate` = `FUN_005A5540`.
  At the end of `ePhysics::update` the game sets the actor's rate from `physics+0x790` (script message `setAnimRate`). Script transitions like `when anim.finished()` are driven by this time.

## Input and camera
- Pad objects (vtable `0x983E34`; pad 0 at `0xB3BBE0`): `+4/+8` left stick X/Y (forward = +Y, right = +X), `+0xC/+0x10` right stick. vtable slots: `[2] isDown(idx)`, `[3] isUp(idx)`, `[6..9]` getLX/LY/RX/RY.
  Action indices follow the `KF_*` binding order: 0 JUMP, 1 PUNCH, 2 ROLL, 3 THROW, 4 STRAFE, ...; the keyboard path (`FUN_00565BF0`) fills them from DirectInput. Bindings: `[qwerty]` section, `KF_*_key/pad/mouse`.
- The game contains mouse code (`FUN_00564BA0`) that nothing calls; the camera (`FUN_004BDD10`, eFollowCameraCtrl) reads the right stick with a 0.4 dead zone, smooths it over 4 frames and integrates it as an angular velocity (fields `+0x3C8/+0x3CC` of eGameCamera).
- The window procedure is replaced by the game (DirectInput), so key messages are unreliable; poll with `GetAsyncKeyState`.

## File formats
- **PAK** (`TATE`): header, then `item` entries (`"item"`, u32 size, 8 bytes, 0x70-byte name) aligned to 0x80; `u16 @6` = size of the main section in 64 KB blocks; language sections follow the main section. See `tools/tools_pak*.py`.
- **ini files** (`levels.ini`, `items.ini`, ...): XOR with a fixed key (`tools/tools_inicrypt.py`). `levels.ini` entries: `+level:"<file>; <title>; <id>; <aliases...>"`.
- **.ar scene archives** (version `0x89`): header (magic, version, ref count), then objects. Every reference is tagged: `0` = new object (followed by a u32 type id), `1` = NULL, `n` = reference to registration `n-2`.
  Strings, tree nodes and special `3fX` arrays share the same registration table. Field order per class = the class's `serialize()`; the readers/writers in `tools/` cover the classes needed for static levels.
- **Scripts** (`.def`): `state` / `gadget` / `when` / `shift` / `after` constructs; found in the PAKs under `characters/` and `scripts/`.
- **Levels**: `media/<name>.PAK` containing `build/win32/<name>.ar`, `levels/<name>/loading.bmp`, `levels/<name>/misc.ini`; the generic hero script (`denis_level05.def`) is taken from the donor level.

## Function quick reference
| address | role |
|---|---|
| `0x4CB9A0` | `ePhysics::update` |
| `0x5A4C20` / `0x5A54C0` / `0x5A5540` | `eAnimate::update` / `getRate` / `setRate` |
| `0x5B42C0` | scene render (called from `0x5709CD` in `FUN_00570980`) |
| `0x5746E0` | begin frame: projection, camera |
| `0x55AB20` | camera lookAt setup |
| `0x54C3F0` | `eTransform::outerDraw` (per-node draw) |
| `0x5741A0` | load `V*M` into `GL_MODELVIEW` for a node |
| `0x47F490` | stencil shadow full-screen pass |
| `0x565BF0` / `0x565060` | keyboard / joystick -> pad 0 |
| `0x4BDD10` | follow-camera input (right stick -> angular velocity) |
| `0x41C9A0` | `loadLevel` message handler |
| `0x597A10`, `0x57C020`, `0x57EF30` | sweep-and-prune pair state change |
| `0x57F160` | `eCollisionMgr::serialize` |
