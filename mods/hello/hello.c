/* Minimal KaoForge mod: an on-screen overlay with a timer and Kao's position, a Home-menu toggle, and Ctrl+H to hide it. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include "kaoforge.h"

static const KaoAPI *K;
static int g_show = 1;
static float g_time;

static void tick(float dt) {
    g_time += dt;
    if (K->key_down(VK_CONTROL) && K->key_pressed('H')) g_show ^= 1;
}

static void draw(int w, int h) {
    (void)w;
    if (!g_show) return;
    char b[128];
    float p[3];
    if (K->hero_pos(p)) snprintf(b, sizeof b, "hello mod: %.0f s   Kao at %.0f %.0f %.0f", g_time, p[0], p[1], p[2]);
    else snprintf(b, sizeof b, "hello mod: %.0f s", g_time);
    glColor4f(0.6f, 1.0f, 0.6f, 1.0f);
    K->text(20.0f, (float)h - 20.0f, b);
}

static const char *label(void) { return g_show ? "[hello] overlay: ON" : "[hello] overlay: off"; }
static void click(int right) { (void)right; g_show ^= 1; }

__declspec(dllexport) int KaoMod_Init(const KaoAPI *api) {
    if (api->version < KAOAPI_VERSION) return 1;
    K = api;
    K->on_tick(tick);
    K->on_draw(draw);
    K->menu_item(label, click);
    K->log("hello: loaded\n");
    return 0;
}
