/*
 * kaoforge.h - KaoForge mod API (v4).
 *
 * A mod is a DLL in <game>/mods/ that exports
 *     __declspec(dllexport) int  KaoMod_Init(const KaoAPI *api);   // return 0 = OK
 *     __declspec(dllexport) void KaoMod_Shutdown(void);            // optional, called before a hot-reload
 * Build and install with:  python3 kaoforge.py mods   (every folder mods/<name>/ with .c files becomes mods/<name>.dll).
 * Everything runs on the game thread, once per frame. Game addresses must go through api->va() (the exe is relocatable).
 * New API versions only append fields to KaoAPI, so older mods keep working; check api->version in KaoMod_Init.
 */
#ifndef KAOFORGE_H
#define KAOFORGE_H
#include <stddef.h>
#define KAOAPI_VERSION 4

typedef void (*KaoTickFn)(float dt);                 /* every frame; dt in seconds */
typedef void (*KaoDrawFn)(int w, int h);             /* 2D drawing: pixel-space ortho (0,0 = top left), after the game's HUD */
typedef void (*KaoDraw3DFn)(void);                   /* 3D drawing in the game world: GL_PROJECTION = P, GL_MODELVIEW = V (camera),
                                                        so draw in world coordinates; the scene's depth buffer is active; after the scene, before the HUD */
typedef const char *(*KaoLabelFn)(void);             /* text of a row in the Home menu */
typedef void (*KaoClickFn)(int right_half);          /* click on a menu row (1 = right half of the row) */
typedef void (*KaoCmdFn)(int argc, char **argv);     /* console command; argv[0] = command name */
typedef int  (*KaoWalkFn)(void *node, int depth, void *ctx);   /* return 0 = continue, 1 = stop, 2 = skip children */
typedef void (*KaoLevelFn)(const char *level);       /* after a level has loaded (scene and hero are ready) */
typedef int  (*KaoKeyFn)(int vk, int down);          /* key message from the game window; return 1 to swallow it */
typedef void (*KaoShutdownFn)(void);                 /* optional export KaoMod_Shutdown */

typedef struct KaoAPI {
    int version;
    /* logging (kaoforge.log) */
    void (*log)(const char *fmt, ...);
    /* addresses */
    unsigned (*va)(unsigned addr);                   /* game address (as in the preferred image base) -> address in this process */
    int (*readable)(const void *p, size_t n);        /* safe readability check */
    /* game objects */
    void *(*hero)(void);                             /* Kao's actor (local position (float*)(hero+0x58), world position +0x78) */
    void *(*scene)(void);                            /* current eScene */
    const char *(*node_name)(void *node);
    const char *(*class_name)(void *obj);            /* RTTI name, e.g. ".?AVeTriMesh@@" */
    int (*is_a)(void *obj, const char *cls);         /* RTTI check including base classes, e.g. is_a(n, "eGroup") */
    void *(*find_node)(const char *name);            /* first scene node with this name */
    void (*load_level)(const char *name);            /* name from levels.ini */
    /* registration */
    void (*on_tick)(KaoTickFn fn);
    void (*on_draw)(KaoDrawFn fn);
    void (*menu_item)(KaoLabelFn label, KaoClickFn click);
    /* input and drawing */
    int (*key_pressed)(int vk);                      /* rising edge (VK_*) */
    int (*key_down)(int vk);
    void (*text)(float x, float y, const char *s);   /* only inside on_draw; colour via glColor4f */
    /* --- v2 --- */
    void (*on_draw3d)(KaoDraw3DFn fn);
    int (*punch)(void);                              /* Kao is punching this frame (game's punch button or left mouse button) */
    int (*menu_open)(void);                          /* the Home menu is open (the game should not react) */

    /* --- v3: hooks and memory (pass game code addresses through api->va(0x...)) ---
     * Everything a mod installs is undone on hot-reload (console: reload). Functions return 0 = OK. */
    int   (*hook)(void *target, void *detour, void **orig);      /* inline detour of any function; *orig = trampoline */
    int   (*hook_vtable)(void **slot, void *fn, void **orig);    /* replace a pointer (vtable slot, function table) */
    int   (*hook_import)(const char *dll, const char *func, void *fn, void **orig);   /* import table (IAT) of the game exe */
    int   (*hook_call)(void *call_site, void *fn, void **orig);  /* retarget an E8 (call rel32) instruction */
    int   (*patch)(void *addr, const void *bytes, size_t n);     /* byte patch (undone on reload) */
    int   (*write)(void *dst, const void *src, size_t n);        /* write with VirtualProtect (not undone) */
    /* calling game functions: any convention (cdecl / stdcall / thiscall - `self` goes in ECX and may be NULL) */
    unsigned (*call)(void *fn, void *self, int argc, const unsigned *argv);
    float    (*call_f)(void *fn, void *self, int argc, const unsigned *argv);  /* result in ST0 */

    /* scene and objects */
    void  (*walk)(void *root, KaoWalkFn cb, void *ctx);          /* root NULL = current scene */
    int   (*children)(void *node, void ***list);                 /* number of children (eGroup); *list = child array */
    int   (*node_pos)(void *node, float *out3);                  /* world position (eTransform +0x78, otherwise the bounding sphere centre +0x20) */
    int   (*node_set_pos)(void *node, const float *p3);          /* eTransform: local +0x58 and world +0x78 */
    void  (*node_set_visible)(void *node, int on);               /* draw bit 0x10000000 in the node flags (+0x1c) */
    void  (*node_set_collision)(void *node, int on);             /* collision mask +0x30: 0x60 = no collision */
    int   (*hero_pos)(float *out3);
    int   (*hero_teleport)(const float *p3);
    const char *(*level)(void);                                  /* name of the current level (loaded through KaoForge) or "?" */

    /* events */
    void  (*on_level)(KaoLevelFn fn);
    void  (*on_key)(KaoKeyFn fn);
    void  (*input_block)(int on);                                /* cut off the game's pad/keyboard input (sticks and action buttons) */

    /* drawing */
    int   (*world_to_screen)(const float *w3, float *sx, float *sy);   /* 1 = in front of the camera; pixels as in on_draw */
    void  (*rect)(float x0, float y0, float x1, float y1);       /* filled rectangle (on_draw; colour via glColor4f) */

    /* in-game console (key ` or F10) */
    void  (*command)(const char *name, const char *help, KaoCmdFn fn);
    void  (*print)(const char *fmt, ...);                        /* print to the console (and the log) */
    int   (*exec)(const char *line);                             /* run a console line */

    /* configuration: <game>/kaoforge_mods.ini, section = usually the mod name */
    int         (*cfg_int)(const char *sec, const char *key, int def);
    float       (*cfg_float)(const char *sec, const char *key, float def);
    const char *(*cfg_str)(const char *sec, const char *key, const char *def);   /* buffer valid until the next call */
    void        (*cfg_set)(const char *sec, const char *key, const char *val);
    const char *(*game_dir)(void);

    /* --- v4 --- */
    int   (*camera)(float *eye3, float *fwd3);                   /* camera position and view direction (from the last frame's P*V), 1 = OK */
} KaoAPI;

#ifdef __cplusplus
extern "C"
#endif
__declspec(dllexport) int KaoMod_Init(const KaoAPI *api);
#endif
