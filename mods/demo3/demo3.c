/* Forge API v3/v4 demo: console commands, config, labels over scene objects (world_to_screen), the level-loaded event and a game-function hook.
 * Console (key ` or F10): labels [0/1], goto <name>, where, hookdemo.
 * Labels show the names of meshes whose name starts with "barrel" (the sample maps contain some); change PREFIX to label other nodes. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "kaoforge.h"

#define PREFIX "barrel"
static const KaoAPI *K;
static int g_labels;
static float g_pos[64][3]; static char g_names[64][32]; static int g_n;

static int collect_cb(void *node, int depth, void *ctx) {
    (void)depth; (void)ctx;
    const char *nm = K->node_name(node);
    if (g_n < 64 && !strncmp(nm, PREFIX, strlen(PREFIX)) && K->is_a(node, "eTriMesh")) {
        float *bb = (float *)((char *)node + 0x40);                 /* mesh bounding box: min3, max3 (world space for static geometry) */
        g_pos[g_n][0] = (bb[0] + bb[3]) / 2; g_pos[g_n][1] = (bb[1] + bb[4]) / 2; g_pos[g_n][2] = bb[5] + 20;
        snprintf(g_names[g_n++], 32, "%.31s", nm);
    }
    return 0;
}
static void on_level(const char *lvl) {
    g_n = 0; K->walk(NULL, collect_cb, NULL);
    K->log("demo3: level %s, %d labelled nodes\n", lvl, g_n);
}
static void draw(int w, int h) {
    (void)w; (void)h;
    if (!g_labels) return;
    float hp[3]; if (!K->hero_pos(hp)) return;
    for (int i = 0; i < g_n; i++) {
        float dx = g_pos[i][0] - hp[0], dy = g_pos[i][1] - hp[1];
        if (dx * dx + dy * dy > 1500.0f * 1500.0f) continue;        /* only nearby */
        float sx, sy;
        if (!K->world_to_screen(g_pos[i], &sx, &sy)) continue;
        glColor4f(1.0f, 0.85f, 0.3f, 1.0f);
        K->text(sx - 30, sy, g_names[i]);
    }
}
static void c_labels(int c, char **v) { g_labels = c > 1 ? atoi(v[1]) != 0 : !g_labels; K->cfg_set("demo3", "labels", g_labels ? "1" : "0"); K->print("labels = %d", g_labels); }
static void c_goto(int c, char **v) {
    (void)c; (void)v;
    if (!g_n) { K->print("no labelled nodes (load a map that has some)"); return; }
    float p[3] = { g_pos[0][0] + 150, g_pos[0][1], g_pos[0][2] + 50 };
    K->hero_teleport(p); K->print("teleported next to %s", g_names[0]);
}
static void c_where(int c, char **v) {
    (void)c; (void)v; float p[3]; K->hero_pos(p);
    K->print("level %s, Kao %.0f %.0f %.0f, game dir %s", K->level(), p[0], p[1], p[2], K->game_dir());
}
/* Hook demo: put a detour on a game function and count its calls. The hook is removed automatically by `reload`. */
static unsigned (*orig_fn)(void);
static int g_calls;
static unsigned det(void) { g_calls++; return orig_fn(); }
static void c_hookdemo(int c, char **v) {
    (void)c; (void)v;
    if (orig_fn) { K->print("hook already installed, calls so far: %d", g_calls); return; }
    int r = K->hook((void *)K->va(0x005732f0u), (void *)det, (void **)&orig_fn);   /* a small function that reads a debug flag */
    K->print("hook: %s", r == 0 ? "OK (run hookdemo again to read the counter)" : "failed");
}

__declspec(dllexport) int KaoMod_Init(const KaoAPI *api) {
    if (api->version < 3) return 1;
    K = api;
    g_labels = K->cfg_int("demo3", "labels", 1);
    K->on_level(on_level);
    K->on_draw(draw);
    K->command("labels", "labels over nodes [0/1]", c_labels);
    K->command("goto", "teleport next to the first labelled node", c_goto);
    K->command("where", "level and Kao's position", c_where);
    K->command("hookdemo", "demo: detour a game function and count calls", c_hookdemo);
    K->log("demo3: loaded\n");
    return 0;
}
__declspec(dllexport) void KaoMod_Shutdown(void) { orig_fn = NULL; g_calls = 0; }
