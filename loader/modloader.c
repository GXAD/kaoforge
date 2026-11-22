/*
 * modloader.c - KaoForge mod loader (included from kaoforge.c after the helpers, before the menu).
 * Loads <game>/mods/<name>.dll and calls KaoMod_Init(&api). API: ../include/kaoforge.h.
 */
#include "../include/kaoforge.h"

#define FORGE_MAX 32
static KaoTickFn g_ftick[FORGE_MAX]; static int g_nftick;
static KaoDrawFn g_fdraw[FORGE_MAX]; static int g_nfdraw;
static KaoDraw3DFn g_fdraw3[FORGE_MAX]; static int g_nfdraw3;
static KaoLabelFn g_flabel[FORGE_MAX]; static KaoClickFn g_fclick[FORGE_MAX]; static int g_nfmenu;
static int g_nmods;
static void text_at(float x, float y, const char *s);
static int font_ready(void);

static void f_log(const char *fmt, ...) { if (!g_log) return; va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log); }
static unsigned f_va(unsigned a) { return (unsigned)VA(a); }
static int f_readable(const void *p, size_t n) { return readable(p, n); }
static void *f_hero(void) { return get_hero(); }
static void *f_scene(void) {
    void **appp = (void **)VA(ADDR_APP);
    return (readable(appp, 4) && *appp) ? *(void **)((char *)*appp + 0xc) : NULL;
}
static const char *f_node_name(void *n) { return node_name((char *)n); }
static const char *f_class(void *o) { return rtti_name(o); }
static int f_is_a(void *o, const char *c) { return is_a(o, c); }
static void *f_find(const char *name) { int b = 300000; char *s = (char *)f_scene(); return s ? find_named(s, name, 0, &b) : NULL; }
static void f_load(const char *n) { load_level(n); }
static void f_on_tick(KaoTickFn f) { if (f && g_nftick < FORGE_MAX) g_ftick[g_nftick++] = f; }
static void f_on_draw(KaoDrawFn f) { if (f && g_nfdraw < FORGE_MAX) g_fdraw[g_nfdraw++] = f; }
static void f_menu(KaoLabelFn l, KaoClickFn c) { if (l && g_nfmenu < FORGE_MAX) { g_flabel[g_nfmenu] = l; g_fclick[g_nfmenu++] = c; } }
static int f_key_pressed(int vk) { return pressed(vk & 0xff); }
static int f_key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static void f_text(float x, float y, const char *s) { if (font_ready()) text_at(x, y, s); }
static void f_on_draw3d(KaoDraw3DFn f) { if (f && g_nfdraw3 < FORGE_MAX) g_fdraw3[g_nfdraw3++] = f; }
static int f_punch(void) {
    if (g_menu) return 0;
    return lmb_held() || (orig_isDown && orig_isDown((void *)VA(ADDR_PAD0), ACT_PUNCH));
}
static int f_menu_open(void) { return g_menu; }

#include "modapi.c"

static const KaoAPI g_api = {
    KAOAPI_VERSION, f_log, f_va, f_readable, f_hero, f_scene, f_node_name, f_class, f_is_a, f_find, f_load,
    f_on_tick, f_on_draw, f_menu, f_key_pressed, f_key_down, f_text,
    f_on_draw3d, f_punch, f_menu_open,
    /* v3 */
    f_hook, f_hook_vtable, f_hook_import, f_hook_call, f_patch, f_write, f_call, f_call_f,
    f_walk, f_children, f_node_pos, f_node_set_pos, f_node_set_visible, f_node_set_collision, f_hero_pos, f_hero_tp, f_level,
    f_on_level, f_on_key, f_input_block,
    f_w2s, f_rect,
    f_command, f_print, f_exec,
    f_cfg_int, f_cfg_float, f_cfg_str, f_cfg_set, f_game_dir,
    f_camera,
};

#define FMOD_MAX 32
static HMODULE g_modh[FMOD_MAX]; static char g_modname[FMOD_MAX][64]; static int g_nmodh;
static void c_mods(int c, char **v) {
    (void)c; (void)v;
    for (int i = 0; i < g_nmodh; i++) f_print("%s", g_modname[i]);
    f_print("mods: %d, tick %d, draw %d, draw3d %d, menu %d, commands %d, patches %d", g_nmodh, g_nftick, g_nfdraw, g_nfdraw3, g_nfmenu, g_ncmd, g_nfp);
}

static int g_mods_loaded;
static void forge_load_mods(void) {
    if (g_mods_loaded) return;
    g_mods_loaded = 1;
    static int builtins; if (!builtins) { builtins = 1; forge_builtin_commands(); }
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA("mods\\*.dll", &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("forge: no mods (mods/ folder)\n"); return; }
    do {
        char path[MAX_PATH + 16]; snprintf(path, sizeof path, "mods\\%s", fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        if (!m) { logf_("forge: %s - cannot load (error %lu)\n", fd.cFileName, GetLastError()); continue; }
        int (*init)(const KaoAPI *) = (int (*)(const KaoAPI *))GetProcAddress(m, "KaoMod_Init");
        if (!init) { logf_("forge: %s - no KaoMod_Init\n", fd.cFileName); FreeLibrary(m); continue; }
        int r = init(&g_api);
        logf_("forge: %s - init %s\n", fd.cFileName, r == 0 ? "OK" : "FAILED");
        if (r == 0 && g_nmodh < FMOD_MAX) { g_modh[g_nmodh] = m; snprintf(g_modname[g_nmodh++], 64, "%.60s", fd.cFileName); g_nmods++; }
        else FreeLibrary(m);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf_("forge: mods: %d (tick %d, draw %d, menu %d, commands %d)\n", g_nmods, g_nftick, g_nfdraw, g_nfmenu, g_ncmd);
    if (GetFileAttributesA("autoexec.cfg") != INVALID_FILE_ATTRIBUTES) { char *a[2] = { "exec", "autoexec.cfg" }; c_exec(2, a); }
}

/* hot-reload: KaoMod_Shutdown, undo the mods' patches/hooks, clear registrations, FreeLibrary, load again */
static void forge_reload(void) {
    for (int i = 0; i < g_nmodh; i++) {
        KaoShutdownFn sd = (KaoShutdownFn)GetProcAddress(g_modh[i], "KaoMod_Shutdown");
        if (sd) sd();
    }
    fp_restore_mods();
    g_nftick = g_nfdraw = g_nfdraw3 = g_nfmenu = g_nflevel = g_nfkey = 0;
    g_mod_block = 0;
    int k = 0; for (int i = 0; i < g_ncmd; i++) if (g_cmd[i].builtin) g_cmd[k++] = g_cmd[i];
    g_ncmd = k;
    for (int i = 0; i < g_nmodh; i++) FreeLibrary(g_modh[i]);
    f_print("reload: unloaded %d mods", g_nmodh);
    g_nmodh = 0; g_nmods = 0; g_mods_loaded = 0;
    forge_load_mods();
    f_print("reload: loaded %d mods", g_nmods);
}

static void forge_tick(float dt) {
    forge_subclass();
    if (g_dist_dirty && GetTickCount() - g_dist_dirty > 800) { g_dist_dirty = 0; settings_save(); }
    forge_console_poll();
    forge_level_watch();
    for (int i = 0; i < g_nftick; i++) g_ftick[i](dt);
}

static void forge_draw(void) {
    if (!g_nfdraw && !g_con_open) return;
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0) return;
    GLint bound; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glPushAttrib(GL_ALL_ATTRIB_BITS); glPushAttrib(GL_LIST_BIT);
    if (p_UseProgram) p_UseProgram(0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, vp[2], vp[3], 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    for (int i = 0; i < g_nfdraw; i++) g_fdraw[i](vp[2], vp[3]);
    forge_console_draw(vp[2], vp[3]);
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib(); glPopAttrib();
    glBindTexture(GL_TEXTURE_2D, bound);
}

/* 3D: called from the glLoadMatrixf hook when returning to 0x5b4339 (FUN_005b42c0, after the scene is drawn, before the HUD).
 * At that point GL_PROJECTION = P and GL_MODELVIEW = V (camera); the scene depth buffer is intact. */
static void forge_draw3d(void) {
    if (!g_nfdraw3) return;
    GLint bound; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (p_UseProgram) p_UseProgram(0);
    glMatrixMode(GL_MODELVIEW); glPushMatrix();            /* MODELVIEW = camera V: draw in world coordinates */
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glColorMask(1, 1, 1, 1);
    for (int i = 0; i < g_nfdraw3; i++) g_fdraw3[i]();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib();
    glBindTexture(GL_TEXTURE_2D, bound);
}
typedef void (APIENTRY *PFN_LoadMatrixf2)(const GLfloat *);
static PFN_LoadMatrixf2 real_glLoadMatrixf2;
static void APIENTRY forge_hook_glLoadMatrixf(const GLfloat *m) {
    real_glLoadMatrixf2(m);
    if ((UINT_PTR)__builtin_return_address(0) == VA(0x005B4339u)) { forge_capture_pv(); forge_draw3d(); }
}
static void forge_install_hooks(void) {
    void *lm = iat_hook(GetModuleHandleA(NULL), "OPENGL32.dll", "glLoadMatrixf", (void *)forge_hook_glLoadMatrixf);
    if (lm) real_glLoadMatrixf2 = (PFN_LoadMatrixf2)lm;
    logf_("forge: 3D hook (glLoadMatrixf) %s\n", lm ? "OK" : "MISSING");
}
