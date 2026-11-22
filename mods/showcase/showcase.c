/*
 * "showcase" - demonstrates what the KaoForge API can do (API v4):
 *   - TELEMETRY: a detour on the game's node-drawing function (FUN_0054c3f0, eTransform::outerDraw) counts nodes drawn per frame;
 *     a corner HUD shows FPS, nodes drawn, scene size and Kao's position.
 *   - INSPECTOR (Ctrl+I or the `inspect` command): a crosshair in the screen centre; the mod walks the scene tree, projects object
 *     bounding boxes to the screen (world_to_screen), picks the smallest box under the crosshair, shows its name/class/size
 *     and draws a wireframe box in the 3D world (on_draw3d, depth test off so it shows through walls).
 *       E = remove the object from the scene (hidden + no collision)   R = restore everything that was removed
 *   - RING: a pulsing ring drawn in 3D around Kao (off by default).
 *   - Commands: inspect [0/1], highlight [0/1], restore, stats [0/1], ring [0/1], showcase [0/1]. Settings are stored in kaoforge_mods.ini ([showcase]).
 * Note: bounding boxes are in world space only for static geometry (e.g. the grid map); in the game's own levels props under
 * transforms have local boxes, so the inspector will show them in the wrong places.
 */
#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kaoforge.h"

static const KaoAPI *K;
static int g_inspect, g_ring, g_stats = 1;
static int g_nodes_frame, g_nodes_last, g_scene_nodes, g_frames;
static float g_fps, g_acc;
static void (__attribute__((thiscall)) *orig_draw)(void *, void *);

static void __attribute__((thiscall)) det_draw(void *self, void *ctx) { g_nodes_frame++; orig_draw(self, ctx); }

#define MAXC 600
typedef struct { void *node; float bb[6]; char name[48]; char cls[24]; int removed; } Cand;
static Cand g_c[MAXC]; static int g_nc;
static Cand *g_pick;
static void *g_removed[64]; static int g_nrem;

static int count_cb(void *n, int d, void *c) { (void)n; (void)d; (*(int *)c)++; return 0; }
typedef struct { float hx, hy; } Near;
static int collect_cb(void *n, int depth, void *ctx) {
    (void)depth; Near *h = (Near *)ctx;
    if (g_nc >= MAXC || !K->is_a(n, "eGeometry")) return 0;
    float *bb = (float *)((char *)n + 0x40);
    for (int i = 0; i < 6; i++) if (!(fabsf(bb[i]) < 1e7f)) return 0;
    if (bb[3] <= bb[0] || bb[4] <= bb[1]) return 0;
    float cx = (bb[0] + bb[3]) * 0.5f, cy = (bb[1] + bb[4]) * 0.5f;
    float hx = bb[3] - bb[0], hy = bb[4] - bb[1];
    if (hx > 30000 || hy > 30000) return 0;                                  /* skip the floor and other giants */
    if (fabsf(cx - h->hx) - hx * 0.5f > 3500 || fabsf(cy - h->hy) - hy * 0.5f > 3500) return 0;
    Cand *c = &g_c[g_nc++]; c->node = n; memcpy(c->bb, bb, 24); c->removed = 0;
    snprintf(c->name, sizeof c->name, "%.47s", K->node_name(n));
    const char *cl = K->class_name(n); snprintf(c->cls, sizeof c->cls, "%.23s", (cl && strlen(cl) > 4) ? cl + 4 : cl);
    char *at = strchr(c->cls, '@'); if (at) *at = 0;
    return 0;
}

static void tick(float dt) {
    g_frames++; g_acc += dt;
    g_nodes_last = g_nodes_frame; g_nodes_frame = 0;
    if (dt > 0) g_fps = g_fps * 0.9f + 0.1f / dt;
    if (K->key_down(VK_CONTROL) && K->key_pressed('I')) { g_inspect ^= 1; K->cfg_set("showcase", "inspect", g_inspect ? "1" : "0"); }
    if (g_frames % 120 == 0) { g_scene_nodes = 0; K->walk(NULL, count_cb, &g_scene_nodes); }
    if (!g_inspect) return;
    if (g_frames % 20 == 0) {
        float hp[3];
        if (K->hero_pos(hp)) { Near h = { hp[0], hp[1] }; g_nc = 0; g_pick = NULL; K->walk(NULL, collect_cb, &h); }
    }
    if (K->key_pressed('R')) {
        for (int i = 0; i < g_nrem; i++) { K->node_set_visible(g_removed[i], 1); K->node_set_collision(g_removed[i], 1); }
        K->print("restored %d objects", g_nrem); g_nrem = 0; g_pick = NULL;
    }
    if (K->key_pressed('E') && g_pick && g_nrem < 64) {
        K->node_set_visible(g_pick->node, 0); K->node_set_collision(g_pick->node, 0);
        g_removed[g_nrem++] = g_pick->node; g_pick->removed = 1;
        K->print("removed: %s (%s)", g_pick->name, g_pick->cls); g_pick = NULL;
    }
}

/* smallest box whose screen projection contains the screen centre */
static int project_box(const float *bb, float *minx, float *miny, float *maxx, float *maxy) {
    *minx = *miny = 1e9f; *maxx = *maxy = -1e9f;
    for (int i = 0; i < 8; i++) {
        float p[3] = { bb[(i & 1) ? 3 : 0], bb[(i & 2) ? 4 : 1], bb[(i & 4) ? 5 : 2] }, sx, sy;
        if (!K->world_to_screen(p, &sx, &sy)) return 0;
        if (sx < *minx) *minx = sx; if (sx > *maxx) *maxx = sx; if (sy < *miny) *miny = sy; if (sy > *maxy) *maxy = sy;
    }
    return 1;
}
static void pick_now(int w, int h) {
    g_pick = NULL; float best = 1e30f, cx = w * 0.5f, cy = h * 0.5f;
    for (int i = 0; i < g_nc; i++) {
        Cand *c = &g_c[i]; if (c->removed) continue;
        float a, b, d, e;
        if (!project_box(c->bb, &a, &b, &d, &e)) continue;
        if (cx < a || cx > d || cy < b || cy > e) continue;
        float vol = (c->bb[3] - c->bb[0]) * (c->bb[4] - c->bb[1]) * (c->bb[5] - c->bb[2] + 1.0f);
        if (vol < best) { best = vol; g_pick = c; }
    }
}

static void draw(int w, int h) {
    char b[160];
    if (g_stats) {
        float hp[3] = { 0, 0, 0 }; int has = K->hero_pos(hp);
        glColor4f(0, 0, 0, 0.55f); K->rect((float)w - 430, 10, (float)w - 10, 120);
        glColor4f(0.6f, 1.0f, 0.7f, 1);
        snprintf(b, sizeof b, "KaoForge  %.0f FPS   level %s", g_fps, K->level()); K->text((float)w - 420, 36, b);
        snprintf(b, sizeof b, "nodes drawn/frame: %d   scene: %d", g_nodes_last, g_scene_nodes); K->text((float)w - 420, 62, b);
        if (has) snprintf(b, sizeof b, "Kao %.0f %.0f %.0f", hp[0], hp[1], hp[2]); else snprintf(b, sizeof b, "Kao: n/a");
        K->text((float)w - 420, 88, b);
        K->text((float)w - 420, 112, g_inspect ? "inspector: ON (Ctrl+I)" : "inspector: off (Ctrl+I)");
    }
    if (!g_inspect) return;
    pick_now(w, h);
    glColor4f(1, 1, 1, 0.9f);
    K->rect(w * 0.5f - 12, h * 0.5f - 1, w * 0.5f + 12, h * 0.5f + 1); K->rect(w * 0.5f - 1, h * 0.5f - 12, w * 0.5f + 1, h * 0.5f + 12);
    if (g_pick) {
        glColor4f(0, 0, 0, 0.7f); K->rect(w * 0.5f + 24, h * 0.5f - 70, w * 0.5f + 470, h * 0.5f + 36);
        glColor4f(1.0f, 0.85f, 0.3f, 1);
        K->text(w * 0.5f + 34, h * 0.5f - 42, g_pick->name);
        snprintf(b, sizeof b, "%s  %.0f x %.0f x %.0f", g_pick->cls, g_pick->bb[3] - g_pick->bb[0], g_pick->bb[4] - g_pick->bb[1], g_pick->bb[5] - g_pick->bb[2]);
        K->text(w * 0.5f + 34, h * 0.5f - 14, b);
        glColor4f(0.7f, 0.9f, 1, 1); K->text(w * 0.5f + 34, h * 0.5f + 20, "E = remove   R = restore all");
    }
    snprintf(b, sizeof b, "inspector: %d objects nearby, %d removed", g_nc, g_nrem);
    glColor4f(1, 1, 1, 1); K->text(20, (float)h - 20, b);
}

static void draw3d(void) {
    float hp[3];
    if (g_ring && K->hero_pos(hp)) {
        float pulse = 0.5f + 0.5f * sinf(g_acc * 3.0f), r = 140.0f + 40.0f * pulse;
        glLineWidth(3); glColor4f(0.3f, 1.0f, 0.5f, 1);
        glBegin(GL_LINE_LOOP); for (int i = 0; i < 48; i++) { float a = i * 6.2831853f / 48; glVertex3f(hp[0] + cosf(a) * r, hp[1] + sinf(a) * r, hp[2] + 6.0f); } glEnd();
    }
    if (g_inspect && g_pick) {
        const float *b = g_pick->bb;
        glDisable(GL_DEPTH_TEST); glLineWidth(2); glColor4f(1.0f, 0.85f, 0.2f, 1);
        glBegin(GL_LINES);
        for (int i = 0; i < 8; i++) for (int ax = 0; ax < 3; ax++) if (!(i & (1 << ax))) {
            int j = i | (1 << ax);
            glVertex3f(b[(i & 1) ? 3 : 0], b[(i & 2) ? 4 : 1], b[(i & 4) ? 5 : 2]);
            glVertex3f(b[(j & 1) ? 3 : 0], b[(j & 2) ? 4 : 1], b[(j & 4) ? 5 : 2]);
        }
        glEnd();
    }
}

static void c_inspect(int c, char **v) { g_inspect = c > 1 ? atoi(v[1]) != 0 : !g_inspect; K->cfg_set("showcase", "inspect", g_inspect ? "1" : "0"); K->print("inspector = %d", g_inspect); }
static void c_restore(int c, char **v) { (void)c; (void)v; for (int i = 0; i < g_nrem; i++) { K->node_set_visible(g_removed[i], 1); K->node_set_collision(g_removed[i], 1); } K->print("restored %d", g_nrem); g_nrem = 0; g_pick = NULL; }
static void c_ring(int c, char **v) { g_ring = c > 1 ? atoi(v[1]) != 0 : !g_ring; K->cfg_set("showcase", "ring", g_ring ? "1" : "0"); K->print("ring = %d", g_ring); }
static void c_stats(int c, char **v) { g_stats = c > 1 ? atoi(v[1]) != 0 : !g_stats; K->cfg_set("showcase", "stats", g_stats ? "1" : "0"); K->print("stats = %d", g_stats); }
static void c_all(int c, char **v) {                    /* showcase [0/1] - everything at once: inspector, ring, stats */
    int on = c > 1 ? atoi(v[1]) != 0 : !(g_inspect || g_ring || g_stats);
    g_inspect = g_ring = g_stats = on; g_pick = NULL;
    K->cfg_set("showcase", "inspect", on ? "1" : "0"); K->cfg_set("showcase", "ring", on ? "1" : "0"); K->cfg_set("showcase", "stats", on ? "1" : "0");
    K->print("showcase: everything %s", on ? "enabled" : "disabled");
}
static const char *label(void) { return g_inspect ? "[showcase] inspector: ON" : "[showcase] inspector: off"; }
static void click(int r) { (void)r; g_inspect ^= 1; }
static void on_level(const char *l) { g_nc = 0; g_pick = NULL; g_nrem = 0; K->log("showcase: level %s\n", l); }

__declspec(dllexport) int KaoMod_Init(const KaoAPI *api) {
    if (api->version < 4) return 1;
    K = api;
    g_inspect = K->cfg_int("showcase", "inspect", 0);
    g_ring = K->cfg_int("showcase", "ring", 0);
    g_stats = K->cfg_int("showcase", "stats", 1);
    int r = K->hook((void *)K->va(0x0054C3F0u), (void *)det_draw, (void **)&orig_draw);   /* eTransform::outerDraw */
    K->log("showcase: outerDraw hook %s\n", r == 0 ? "OK" : "FAILED (telemetry off)");
    K->on_tick(tick); K->on_draw(draw); K->on_draw3d(draw3d); K->on_level(on_level); K->menu_item(label, click);
    K->command("inspect", "object inspector [0/1]  (Ctrl+I; E removes, R restores)", c_inspect);
    K->command("restore", "restore removed objects", c_restore);
    K->command("ring", "ring around Kao [0/1]", c_ring);
    K->command("highlight", "object highlighting (inspector) [0/1]", c_inspect);
    K->command("showcase", "everything from this mod at once [0/1]: inspector, ring, stats", c_all);
    K->command("stats", "stats box [0/1]", c_stats);
    return 0;
}
