/*
 * Kao2 mod – proxy glut32.dll (32-bit).
 *  - glutInit        -> hook (naked, calling convention irrelevant): code patches after the exe has decrypted itself in memory
 *  - glutSwapBuffers -> hook: the per-frame tick (game thread)
 * All other exports are forwarded to glut32_real.dll (glut32.def).
 *
 * Hotkeys (polled with GetAsyncKeyState):
 *   F1  fly (look-direction flight; PageUp / PageDown = up / down; Space is the game's jump)
 *   F2  fast movement (x3)
 *   F3  higher jump
 *   F4  god mode (the game's UNLIMITED_ENERGY flag, 0xB34DEB)
 *   F6  load the custom map 99_grid (levels.ini entry + media/99_grid.PAK)
 *   F11 soft shadows on/off
 *   Mouse = camera.  Home = menu (click with the mouse; settings saved to kaoforge.ini); ` or F10 = console
 *   F8  post-processing on/off (Shift+F12 = memory scan for the hero position)
 *   F9  dump the hero object memory (diagnostics)
 * Log: kaoforge.log obok exe. Offsety: NOTES.md.
 *
 * Key fact: ePhysics::update (FUN_004cb9a0) copies the actor transform at the start of a frame
 * from actor+0x48 (8 dwords: quat x,y,z,w, pos x,y,z, ?) into its own state and writes the new
 * transform back to actor+0x48 at the end. hero+0x78 is only a derived copy -> we write to hero+0x58.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#define ADDR_APP        0x00B3D868u  /* eGameApplication* (pointer)           */
#define OFF_HERO        0x38C        /* app->hero (aktor Kao)                 */
#define OFF_LPOS        0x58         /* hero: local position x,y,z (source)   */
#define OFF_WPOS        0x78         /* hero: derived copy of the position    */
#define ADDR_UNL_ENERGY 0x00B34DEBu  /* bajt: flaga UNLIMITED_ENERGY          */
#define ADDR_SPLASH     0x00416100u  /* FUN_00416100: plansze legal/logo      */

#define ADDR_LVL_COUNT  0x00B34C98u  /* int: number of levels                 */
#define ADDR_LVL_ARRAY  0x00B34CA0u  /* eLevelDesc** tablica                  */
#define OFF_LOADER      0x3F4        /* app+0x3f4: object with a vtable; [0](idx+1) = loadLevel (thiscall) */
#define OFF_LOADFLAG    0x384        /* app+0x384: bajt zerowany przed loadLevel */
#define GRID_LEVEL      "99_grid"

#define FLY_SPEED   500.0f  /* units/s (the game's jump is ~140 units) */
#define MOVE_MULT   3.0f
#define JUMP_MULT   1.6f

/* the exe has DYNAMICBASE - addresses are relative to the actual base (preferred 0x400000) */
static UINT_PTR g_delta;
static int g_unsupported;              /* kao2.exe differs from the tested build: the loader leaves the game alone */
#define KNOWN_EXE_SIZE 8423568u
#define VA(x) ((UINT_PTR)(x) + g_delta)

static HMODULE g_real;
static void (*real_SwapBuffers)(void);
void *g_real_glutInit __attribute__((used));
static FILE *g_log;
static int g_fly, g_fast, g_jump, g_god, g_fast_prev;
static float g_speed = 1.0f, g_flyspd = 1.0f, g_jumph = 1.0f, g_jump_prev_dummy;
static int g_jump_prev;          /* (unused) */
#define SPEED_MIN 0.25f
#define SPEED_MAX 8.0f
static char g_cur_level[64] = "?";   /* last level loaded through the loader/menu/console */
static int forge_input_blocked(void);
static void settings_save(void);
static int g_prevkey[256];
static float g_last[3];
static int g_haveLast;
static DWORD g_lastTick;
static int g_dbg;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fflush(g_log);
}

static int readable(const void *p, size_t n) {
    return p && (UINT_PTR)p > 0x10000 && !IsBadReadPtr(p, n);
}

/* write to game code only if the original bytes match */
static int patch(UINT_PTR va, const void *expect, size_t elen, const void *bytes, size_t len, const char *name) {
    BYTE *p = (BYTE *)VA(va);
    DWORD old;
    if (!readable(p, len) || memcmp(p, expect, elen) != 0) {
        logf_("patch %s: niezgodne bajty, pomijam (%02x %02x %02x)\n", name, p[0], p[1], p[2]);
        return 0;
    }
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) { logf_("patch %s: VirtualProtect fail\n", name); return 0; }
    memcpy(p, bytes, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    logf_("patch %s: OK\n", name);
    return 1;
}

#include "gfx.c"

/* ---- mouse camera ----
 * Game input: pad 0 = object @0xB3BBE0 (vtable 0x983E34): +4/+8 left stick, +0xc/+0x10 right stick.
 * The keyboard (FUN_00565bf0) adds +-1 to the right stick for KF_RLEFT/RRIGHT/RFORWARD/RBACKWARD (arrow keys).
 * The camera (FUN_004bdd10) reads the right stick through vtable[8]/[9] (getRX/getRY), dead zone 0.4
 * with rescaling (FUN_005a60a0, not clamped to 1), 4-frame smoothing; the result is a rotation velocity.
 * The game has DirectInput mouse code (FUN_00564ba0) but nothing calls it.
 * We replace getRX/getRY: game value + mouse movement as a velocity (dx/dt), so angle ~ mouse travel. */
#define ADDR_PAD0      0x00B3BBE0u
#define ADDR_PADVT_RX  0x00983E54u   /* vtable[8] = 0x5670b0 (fld [ecx+0xc]) */
#define ADDR_PADVT_RY  0x00983E58u   /* vtable[9] = 0x5670d0 (fld [ecx+0x10]) */
#define ADDR_GAME_HWND 0x00B3BA68u   /* HWND okna (pad.cpp, FUN_00566390) */
typedef float (__attribute__((thiscall)) *PFN_getf)(void *);
static PFN_getf orig_getRX, orig_getRY;
static int g_freecam = 1, g_fc_init, g_mdx, g_mdy;
static float g_fc_yaw, g_fc_pitch, g_fc_dist = 450.0f;
static int g_dist_saved;               /* distance loaded from settings: do not overwrite it with the game camera's distance */
static int g_mouse_on = 1, g_mouse_inv, g_mouse_invx, g_menu, g_frame, g_mframe = -100, g_mskip, g_mcaptured;
static float g_msens = 1.0f, g_mx, g_my;
static POINT g_mlast;
static LARGE_INTEGER g_mqpc;

static void mouse_release(void) {
    if (!g_mcaptured) return;
    g_mcaptured = 0;
    ClipCursor(NULL);
    while (ShowCursor(TRUE) < 0) {}
}

static void mouse_poll(void) {
    if (g_mframe == g_frame) return;
    g_mframe = g_frame;
    g_mx = g_my = 0; g_mdx = g_mdy = 0;
    HWND hw = readable((void *)VA(ADDR_GAME_HWND), 4) ? *(HWND *)VA(ADDR_GAME_HWND) : NULL;
    if (!g_mouse_on || g_menu || !hw || GetForegroundWindow() != hw) { mouse_release(); return; }
    RECT rc; GetClientRect(hw, &rc);
    POINT c = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    ClientToScreen(hw, &c);
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    double dt = g_mqpc.QuadPart ? (double)(now.QuadPart - g_mqpc.QuadPart) / fq.QuadPart : 0.016;
    g_mqpc = now;
    if (dt < 0.002) dt = 0.002; if (dt > 0.1) dt = 0.1;
    if (!g_mcaptured) {
        g_mcaptured = 1; g_mskip = 2;
        while (ShowCursor(FALSE) >= 0) {}
        RECT sr = rc; MapWindowPoints(hw, NULL, (POINT *)&sr, 2);
        ClipCursor(&sr);
        SetCursorPos(c.x, c.y);
        GetCursorPos(&g_mlast);
        return;
    }
    POINT p; GetCursorPos(&p);
    int dx = p.x - g_mlast.x, dy = p.y - g_mlast.y;
    SetCursorPos(c.x, c.y);
    GetCursorPos(&g_mlast);            /* if warping does not work (Wayland) we measure from the current position */
    if (g_mskip) { g_mskip--; return; }
    g_mdx = dx; g_mdy = dy;
    float k = (float)(0.0011 * g_msens / dt);
    g_mx = -dx * k * (g_mouse_invx ? -1.0f : 1.0f);   /* stick +X = turn left -> minus */
    g_my = dy * k * (g_mouse_inv ? -1.0f : 1.0f);
    if (g_mx > 8) g_mx = 8; if (g_mx < -8) g_mx = -8;
    if (g_my > 8) g_my = 8; if (g_my < -8) g_my = -8;
}

static float stick_in(float m) {      /* inverse of the 0.4 dead zone: camera output = m */
    if (m == 0) return 0;
    return m > 0 ? 0.4f + 0.6f * m : -0.4f + 0.6f * m;
}

float __attribute__((thiscall)) my_getRX(void *self) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 0;
    float v = orig_getRX(self);
    if (self == (void *)VA(ADDR_PAD0) && !g_freecam) { mouse_poll(); v += stick_in(g_mx); }
    return v;
}
float __attribute__((thiscall)) my_getRY(void *self) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 0;
    float v = orig_getRY(self);
    if (self == (void *)VA(ADDR_PAD0) && !g_freecam) { mouse_poll(); v += stick_in(g_my); }
    return v;
}

/* ---- punch on the left mouse button ----
 * Pad buttons: action array @pad+0x24 (0xB3BC04), in KF_* order: 0 JUMP, 1 PUNCH, 2 ROLL, 3 THROW...
 * Read through vtable[2] isDown (0x567010) and vtable[3] isUp (0x567030), thiscall(idx), returning bool. */
#define ADDR_PADVT_DOWN 0x00983E3Cu
#define ADDR_PADVT_UP   0x00983E40u
#define ACT_PUNCH 1
typedef BOOLEAN (__attribute__((thiscall)) *PFN_btn)(void *, int);
static PFN_btn orig_isDown, orig_isUp;
static int g_lmb_punch = 1;

static int lmb_held(void) {
    if (!g_lmb_punch || g_menu) return 0;
    HWND hw = readable((void *)VA(ADDR_GAME_HWND), 4) ? *(HWND *)VA(ADDR_GAME_HWND) : NULL;
    return hw && GetForegroundWindow() == hw && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
}
BOOLEAN __attribute__((thiscall)) my_isDown(void *self, int i) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 0;
    BOOLEAN r = orig_isDown(self, i);
    if (i == ACT_PUNCH && self == (void *)VA(ADDR_PAD0) && lmb_held()) r = 1;
    return r;
}
BOOLEAN __attribute__((thiscall)) my_isUp(void *self, int i) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 1;
    BOOLEAN r = orig_isUp(self, i);
    if (i == ACT_PUNCH && self == (void *)VA(ADDR_PAD0) && lmb_held()) r = 0;
    return r;
}

/* left stick (Kao's movement): vtable[6] getLX 0x567070, [7] getLY 0x567090 - zeroed while input is blocked (console) */
#define ADDR_PADVT_LX 0x00983E4Cu
#define ADDR_PADVT_LY 0x00983E50u
static PFN_getf orig_getLX, orig_getLY;
float __attribute__((thiscall)) my_getLX(void *self) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 0;
    return orig_getLX(self);
}
float __attribute__((thiscall)) my_getLY(void *self) {
    if (self == (void *)VA(ADDR_PAD0) && forge_input_blocked()) return 0;
    return orig_getLY(self);
}

static int patch_ptr(UINT_PTR va, UINT_PTR expect, void *repl, void **orig, const char *name) {
    UINT_PTR *p = (UINT_PTR *)VA(va);
    DWORD old;
    if (!readable(p, 4) || *p != VA(expect)) { logf_("patch %s: niezgodny wpis, pomijam\n", name); return 0; }
    *orig = (void *)*p;
    VirtualProtect(p, 4, PAGE_READWRITE, &old);
    *p = (UINT_PTR)repl;
    VirtualProtect(p, 4, old, &old);
    logf_("patch %s: OK\n", name);
    return 1;
}

/* ---- free orbit camera ----
 * FUN_00570980 calls @0x5709cd: eScene::render(ecx=scene, ctx, camera) = FUN_005b42c0.
 * Camera lookAt (FUN_0055ab20): eye = cam+0x58, target = eye + cam+0x170 (view vector), up = cam+0x17c.
 * We redirect that call: before rendering we write our own eye/direction (angles straight from the mouse, no smoothing). */
#define ADDR_CALL_RENDER 0x005709CDu
#define ADDR_SCENE_RENDER 0x005B42C0u
typedef void (__attribute__((thiscall)) *PFN_render)(void *, void *, void *);
static PFN_render orig_render;
static void *get_hero(void);

void __attribute__((thiscall)) my_render(void *scene, void *ctx, void *cam) {
    char *hero = g_freecam ? (char *)get_hero() : NULL;
    if (hero && readable((char *)cam + 0x58, 0x130)) {
        float *eye = (float *)((char *)cam + 0x58), *fwd = (float *)((char *)cam + 0x170), *up = (float *)((char *)cam + 0x17c);
        float *hp = (float *)(hero + 0x78);
        float t[3] = { hp[0], hp[1], hp[2] + 110.0f };
        if (!g_fc_init) {                       /* start from the game camera position -> no jump */
            float v[3] = { t[0] - eye[0], t[1] - eye[1], t[2] - eye[2] };
            float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (l > 1.0f) { g_fc_yaw = atan2f(v[1], v[0]); g_fc_pitch = asinf(v[2] / l); if (!g_dist_saved) g_fc_dist = l < 200 ? 200 : l > 1500 ? 1500 : l; }
            g_fc_init = 1;
        }
        mouse_poll();
        float r = 0.0025f * g_msens;
        g_fc_yaw   -= g_mdx * r * (g_mouse_invx ? -1.0f : 1.0f);
        g_fc_pitch -= g_mdy * r * (g_mouse_inv ? -1.0f : 1.0f);
        if (g_fc_pitch > 1.40f) g_fc_pitch = 1.40f;
        if (g_fc_pitch < -1.40f) g_fc_pitch = -1.40f;
        float cp = cosf(g_fc_pitch);
        float d[3] = { cp * cosf(g_fc_yaw), cp * sinf(g_fc_yaw), sinf(g_fc_pitch) };   /* from eye to target */
        eye[0] = t[0] - d[0] * g_fc_dist; eye[1] = t[1] - d[1] * g_fc_dist; eye[2] = t[2] - d[2] * g_fc_dist;
        fwd[0] = d[0] * 100.0f; fwd[1] = d[1] * 100.0f; fwd[2] = d[2] * 100.0f;
        up[0] = 0; up[1] = 0; up[2] = 1;
    } else if (!hero) g_fc_init = 0;
    orig_render(scene, ctx, cam);
}

static void patch_call(UINT_PTR site, UINT_PTR expect, void *repl, void **orig, const char *name) {
    BYTE *p = (BYTE *)VA(site);
    if (!readable(p, 5) || p[0] != 0xE8 || (UINT_PTR)(p + 5 + *(INT32 *)(p + 1)) != VA(expect)) {
        logf_("patch %s: niezgodne, pomijam\n", name); return;
    }
    *orig = (void *)VA(expect);
    DWORD old; VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(INT32 *)(p + 1) = (INT32)((BYTE *)repl - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf_("patch %s: OK\n", name);
}

void do_patches(void) __attribute__((used));
static void forge_install_hooks(void);
void do_patches(void) {
    static int done;
    if (done) return;
    done = 1;
    if (g_unsupported) { logf_("unsupported kao2.exe build - loader inactive\n"); return; }
    gfx_install_hooks();
    /* Skip the intro screens: FUN_00416100 (fastcall, ecx=app) -> mov byte [ecx+0x37a0],0 ; ret */
    static const BYTE exp_splash[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    static const BYTE new_splash[] = { 0xC6, 0x81, 0xA0, 0x37, 0x00, 0x00, 0x00, 0xC3 };
    patch(ADDR_SPLASH, exp_splash, sizeof exp_splash, new_splash, sizeof new_splash, "skip_intro");
    patch_ptr(ADDR_PADVT_RX, 0x005670B0u, (void *)my_getRX, (void **)&orig_getRX, "mouse_getRX");
    forge_install_hooks();
    patch_call(ADDR_CALL_RENDER, ADDR_SCENE_RENDER, (void *)my_render, (void **)&orig_render, "freecam_render");
    patch_ptr(ADDR_PADVT_DOWN, 0x00567010u, (void *)my_isDown, (void **)&orig_isDown, "lmb_isDown");
    patch_ptr(ADDR_PADVT_UP,   0x00567030u, (void *)my_isUp,   (void **)&orig_isUp,   "lmb_isUp");
    patch_ptr(ADDR_PADVT_LX, 0x00567070u, (void *)my_getLX, (void **)&orig_getLX, "block_getLX");
    patch_ptr(ADDR_PADVT_LY, 0x00567090u, (void *)my_getLY, (void **)&orig_getLY, "block_getLY");
    patch_ptr(ADDR_PADVT_RY, 0x005670D0u, (void *)my_getRY, (void **)&orig_getRY, "mouse_getRY");
}

/* naked: preserve registers and the stack, apply patches, jump to the real glutInit (cdecl/stdcall irrelevant) */
__attribute__((naked)) void Hook_glutInit(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call _do_patches\n\t"
        "popal\n\t"
        "jmp *_g_real_glutInit\n\t");
}

static void *get_hero(void) {
    void **app = (void **)VA(ADDR_APP);
    if (!readable(app, 4) || !*app) return NULL;
    char *a = (char *)*app;
    if (!readable(a + OFF_HERO, 4)) return NULL;
    char *hero = *(char **)(a + OFF_HERO);
    if (!readable(hero + 0x48, 0x40)) return NULL;
    return hero;
}

static int pressed(int vk) {          /* rising edge */
    int d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int r = d && !g_prevkey[vk];
    g_prevkey[vk] = d;
    return r;
}

static void dump_hero(char *hero) {
    float *f = (float *)(hero + 0x40);
    logf_("--- hero=%p app=%p ---\n", hero, *(void **)VA(ADDR_APP));
    for (int i = 0; i < 0x50; i += 4) {      /* 0x40 .. 0x17F */
        if (!readable(f + i, 16)) break;
        logf_("+0x%03x: %10.3f %10.3f %10.3f %10.3f\n", 0x40 + i * 4, f[i], f[i+1], f[i+2], f[i+3]);
    }
}

static int near3(const float *a, const float *p) {
    return a[0] > p[0]-0.05f && a[0] < p[0]+0.05f && a[1] > p[1]-0.05f && a[1] < p[1]+0.05f &&
           a[2] > p[2]-0.05f && a[2] < p[2]+0.05f;
}

/* Shift+F12: scan RW memory for float triples == the hero position */
static void scan_pos(float *p) {
    float pos[3] = { p[0], p[1], p[2] };
    int hits = 0;
    MEMORY_BASIC_INFORMATION mbi;
    char *a = (char *)0x10000;
    logf_("--- scan pos %.3f %.3f %.3f ---\n", pos[0], pos[1], pos[2]);
    while (a < (char *)0x7ff00000 && VirtualQuery(a, &mbi, sizeof mbi)) {
        char *base = (char *)mbi.BaseAddress;
        SIZE_T sz = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) &&
            sz < 0x8000000) {
            for (SIZE_T o = 0; o + 12 <= sz; o += 4) {
                float *f = (float *)(base + o);
                if ((char *)f == (char *)pos) continue;
                if (near3(f, pos)) {
                    int pair = (o + 0x20 + 12 <= sz) && near3((float *)(base + o + 0x20), pos);
                    if (hits < 300) logf_("hit %p pair+20=%d\n", f, pair);
                    hits++;
                }
            }
        }
        a = base + sz;
        if (a <= base) break;
    }
    logf_("--- scan done, %d hits ---\n", hits);
}

static const char *level_name(int i) {
    char **arr = *(char ***)VA(ADDR_LVL_ARRAY);
    if (!readable(arr + i, 4)) return NULL;
    char *desc = arr[i];
    if (!readable(desc + 0x10, 4)) return NULL;
    char *s = *(char **)(desc + 0x10);
    if (!readable(s + 0xc, 1)) return NULL;
    return s + 0xc;
}

typedef void (__attribute__((thiscall)) *load_fn)(void *self, int arg);

/* load a level the same way as the "loadLevel" message (FUN_0041c9a0) */
static void load_level(const char *want) {
    void **appp = (void **)VA(ADDR_APP);
    if (!readable(appp, 4) || !*appp) { logf_("load: no app\n"); return; }
    char *app = (char *)*appp;
    int n = *(int *)VA(ADDR_LVL_COUNT), idx = -1;
    for (int i = 0; i < n && i < 64; i++) {
        const char *nm = level_name(i);
        if (nm && strcmp(nm, want) == 0) idx = i;
    }
    if (idx < 0) { logf_("load: %s is not in levels.ini\n", want); return; }
    void *obj = app + OFF_LOADER;
    if (!readable(obj, 4) || !readable(*(void **)obj, 4)) { logf_("load: bad loader object\n"); return; }
    load_fn fn = **(load_fn **)obj;
    logf_("load: %s idx=%d -> loadLevel(%d)\n", want, idx, idx + 1);
    *(BYTE *)(app + OFF_LOADFLAG) = 0;
    snprintf(g_cur_level, sizeof g_cur_level, "%s", want);
    fn(obj, idx + 1);
    g_haveLast = 0;
}

/* ---- F7: dump the scene graph (app+0xc = eScene) to kaoforge_scene.txt ---- */
static const char *rtti_name(void *obj) {
    if (!readable(obj, 4)) return "?";
    void **vt = *(void ***)obj;
    if (!readable(vt - 1, 4)) return "?";
    char *col = (char *)vt[-1];
    if (!readable(col + 0xc, 4)) return "?";
    char *td = *(char **)(col + 0xc);
    if (!readable(td + 8, 4)) return "?";
    return td + 8;                                    /* ".?AVeTriMesh@@" */
}

static const char *node_name(char *n) {
    if (!readable(n + 0x14, 4)) return "?";
    char *s = *(char **)(n + 0x14);
    if (!s || !readable(s + 0xc, 1)) return "";
    return s + 0xc;
}

static int is_group(const char *cls) {
    static const char *g[] = { "eGroup@", "eTransform@", "ePivot@", "eScene@", "eEnvironment@", "eWSGroup@",
                               "eBillboard@", "eMultiTransform@", "Actor@", "eProxy@", "eXRefProxy@", "eDepthless@",
                               "eOrtho2D@", "eMenu@", "eTouchSensor@", "eNPCMap@", "eParticleEmiter@", 0 };
    for (int i = 0; g[i]; i++) if (strstr(cls, g[i])) return 1;
    return 0;
}

static FILE *g_dump;
static int g_dumped;

static void dump_node(char *n, int depth) {
    if (!readable(n, 0x48) || g_dumped > 30000 || depth > 40) return;
    g_dumped++;
    const char *cls = rtti_name(n);
    unsigned flags = *(unsigned *)(n + 0x1c);
    unsigned short u30 = *(unsigned short *)(n + 0x30);
    char *albox = *(char **)(n + 0x18);
    float *sph = (float *)(n + 0x20);
    fprintf(g_dump, "%*s%s \"%s\" @%p fl=%08x u30=%x u0c=%x", depth * 2, "", cls, node_name(n), (void *)n,
            flags, u30, *(unsigned *)(n + 0xc));
    if (albox && readable(albox, 4)) fprintf(g_dump, " albox=%s", rtti_name(albox));
    fprintf(g_dump, " sph=(%.0f %.0f %.0f r%.0f)", sph[0], sph[1], sph[2], sph[3]);
    if (strstr(cls, "eTriMesh@") && readable(n + 0x5c, 4)) {
        float *bb = (float *)(n + 0x40);
        char *mat = *(char **)(n + 0x3c), *gs = *(char **)(n + 0x58);
        fprintf(g_dump, " bbox=(%.0f %.0f %.0f / %.0f %.0f %.0f)", bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
        if (readable(mat, 0x30)) fprintf(g_dump, " mat=%s b14=%u", rtti_name(mat), *(unsigned char *)(mat + 0x14));
        if (readable(gs, 0x60)) fprintf(g_dump, " gs.fl=%x gs.nv=%d gs.tree=%p", *(unsigned *)(gs + 8), *(int *)(gs + 0xc),
                                        *(void **)(gs + 0x5c));
    }
    fputc('\n', g_dump);
    if (is_group(cls) && readable(n + 0x48, 4)) {
        int cnt = *(int *)(n + 0x3c);
        char **ch = *(char ***)(n + 0x44);
        if (cnt > 0 && cnt < 5000 && readable(ch, cnt * 4))
            for (int i = 0; i < cnt; i++) dump_node(ch[i], depth + 1);
    }
}

static void dump_scene(void) {
    void **appp = (void **)VA(ADDR_APP);
    if (!readable(appp, 4) || !*appp) return;
    char *scene = *(char **)((char *)*appp + 0xc);
    g_dump = fopen("kaoforge_scene.txt", "a");
    if (!g_dump) return;
    g_dumped = 0;
    fprintf(g_dump, "===== scene %p (app=%p) =====\n", (void *)scene, *appp);
    dump_node(scene, 0);
    fprintf(g_dump, "===== %d nodes =====\n", g_dumped);
    fclose(g_dump);
    logf_("scene dump: %d nodes -> kaoforge_scene.txt\n", g_dumped);
}

/* ---- smashable barrels (grid map: meshes "barrelNN / grid" in #VS-grid/1) ----
 * Smashing = clearing the draw bit 0x10000000 in the node flags (+0x1c) and setting the collision mask +0x30 |= 0x60
 * (NO_HERO | NO_COLLIDE). They respawn after BARREL_RESPAWN ms (when Kao is not standing inside). */
#define MAXBARREL 64
#define BARREL_RESPAWN 12000
typedef struct { char *n; float c[3]; float r, h; DWORD dead; unsigned fl; unsigned short u30; } Barrel;
static Barrel g_bar[MAXBARREL];
static int g_nbar, g_smashed;
static DWORD g_smash_msg;

static char *find_named(char *n, const char *name, int depth, int *budget) {
    if (!readable(n, 0x48) || depth > 40 || --*budget < 0) return NULL;
    if (strcmp(node_name(n), name) == 0) return n;
    if (!is_group(rtti_name(n))) return NULL;
    int cnt = *(int *)(n + 0x3c);
    char **ch = *(char ***)(n + 0x44);
    if (cnt <= 0 || cnt > 5000 || !readable(ch, cnt * 4)) return NULL;
    for (int i = 0; i < cnt; i++) { char *r = find_named(ch[i], name, depth + 1, budget); if (r) return r; }
    return NULL;
}

static void barrels_scan(void) {
    static char *last_scene; static DWORD last;
    DWORD now = GetTickCount();
    void **appp = (void **)VA(ADDR_APP);
    if (!readable(appp, 4) || !*appp) { g_nbar = 0; last_scene = NULL; return; }
    char *scene = *(char **)((char *)*appp + 0xc);
    if (scene == last_scene && (g_nbar > 0 || now - last < 3000)) return;
    last = now; last_scene = scene; g_nbar = 0;
    int budget = 40000;
    char *grid = readable(scene, 0x48) ? find_named(scene, "#VS-grid/1", 0, &budget) : NULL;
    if (!grid) return;
    int cnt = *(int *)(grid + 0x3c);
    char **ch = *(char ***)(grid + 0x44);
    if (cnt <= 0 || cnt > 5000 || !readable(ch, cnt * 4)) return;
    for (int i = 0; i < cnt && g_nbar < MAXBARREL; i++) {
        char *m = ch[i];
        if (!readable(m, 0x60) || strncmp(node_name(m), "barrel", 6) != 0) continue;
        float *bb = (float *)(m + 0x40);
        Barrel *b = &g_bar[g_nbar++];
        b->n = m; b->dead = 0;
        b->c[0] = (bb[0] + bb[3]) / 2; b->c[1] = (bb[1] + bb[4]) / 2; b->c[2] = bb[2];
        b->r = (bb[3] - bb[0]) / 2; b->h = bb[5] - bb[2];
        b->fl = *(unsigned *)(m + 0x1c); b->u30 = *(unsigned short *)(m + 0x30);
    }
    logf_("barrels: %d\n", g_nbar);
}

static void barrels_tick(char *hero) {
    barrels_scan();
    if (!g_nbar || !hero) return;
    DWORD now = GetTickCount();
    float *hp = (float *)(hero + 0x78);
    /* punch: left mouse button or the game's punch button (action state in the pad array) */
    static int prev_punch; static DWORD punch_at;
    int punch = lmb_held() || (readable((void *)VA(ADDR_PAD0 + 0x24 + 4 * ACT_PUNCH), 4) && orig_isDown &&
                               orig_isDown((void *)VA(ADDR_PAD0), ACT_PUNCH));
    if (punch && !prev_punch) punch_at = now;
    prev_punch = punch;
    int hitting = punch_at && now - punch_at < 450;      /* hit window during the punch animation */
    for (int i = 0; i < g_nbar; i++) {
        Barrel *b = &g_bar[i];
        if (!readable(b->n, 0x40)) continue;
        float dx = hp[0] - b->c[0], dy = hp[1] - b->c[1], d = sqrtf(dx*dx + dy*dy);
        int zok = hp[2] > b->c[2] - 60 && hp[2] < b->c[2] + b->h + 40;
        if (!b->dead) {
            if (hitting && now - punch_at > 120 && zok && d < b->r + 110) {
                b->dead = now;
                *(unsigned *)(b->n + 0x1c) &= ~0x10000000u;
                *(unsigned short *)(b->n + 0x30) |= 0x60;
                g_smashed++; g_smash_msg = now;
                logf_("barrel %d smashed (total %d)\n", i + 1, g_smashed);
            }
        } else if (now - b->dead > BARREL_RESPAWN && !(zok && d < b->r + 60)) {
            b->dead = 0;
            *(unsigned *)(b->n + 0x1c) = b->fl;
            *(unsigned short *)(b->n + 0x30) = b->u30;
        }
    }
}

/* ===== export all meshes/textures from the levels (Home menu -> "Export props from ALL levels") =====
 * The loader loads the levels from levels.ini one by one, waits until the scene is up, and writes kaoforge_export/<level>.bin:
 *  "KEXP" u32 version; records: 'T' texture, 'G' geoset, 'M' mesh instance (ancestor chain + world transform).
 * Memory layout (from the code): eBitmap +8 w +0xc h +0x10 vw +0x14 vh +0x1c data +0x20 palette(0x400) +0x24 format +0x28 path;
 * eTransform world: +0x68 quat, +0x78 pos, +0x84 scale; eProxy +0xac path; eGroup children +0x3c/+0x44;
 * eTriMesh +0x3c material, +0x40 bbox, +0x58 geoset; eMaterial +0x10 -> [eTexture] -> +0x10 eBitmap. */
static int is_a(void *obj, const char *base) {           /* RTTI: lista klas bazowych z Class Hierarchy Descriptor */
    if (!readable(obj, 4)) return 0;
    void **vt = *(void ***)obj;
    if (!readable(vt - 1, 4)) return 0;
    char *col = (char *)vt[-1];
    if (!readable(col + 0x10, 4)) return 0;
    char *chd = *(char **)(col + 0x10);
    if (!readable(chd + 0x10, 4)) return 0;
    int n = *(int *)(chd + 8); char **arr = *(char ***)(chd + 0xc);
    if (n <= 0 || n > 64 || !readable(arr, n * 4)) return 0;
    char want[64]; snprintf(want, sizeof want, ".?AV%s@@", base);
    for (int i = 0; i < n; i++) {
        char *bcd = arr[i];
        if (!readable(bcd, 4)) continue;
        char *td = *(char **)bcd;
        if (readable(td + 8, 8) && strcmp(td + 8, want) == 0) return 1;
    }
    return 0;
}
static const char *estr(char *s) { return (s && readable(s + 0xc, 1)) ? s + 0xc : ""; }

static FILE *g_ex; static UINT_PTR g_exseen[65536]; static int g_exnseen, g_exmesh, g_extex, g_exgeo;
static int ex_seen(UINT_PTR a) {
    for (int i = 0; i < g_exnseen; i++) if (g_exseen[i] == a) return 1;
    if (g_exnseen < 65536) g_exseen[g_exnseen++] = a;
    return 0;
}
static void exw(const void *p, size_t n) { fwrite(p, 1, n, g_ex); }
static void exu(unsigned v) { exw(&v, 4); }
static void exs(const char *s) { unsigned short l = (unsigned short)strlen(s); exw(&l, 2); exw(s, l); }
static void ex_blob(const void *p, size_t n) { if (p && n && readable(p, n)) { exu((unsigned)n); exw(p, n); } else exu(0); }

static float bpp_of(unsigned f) {
    switch (f) { case 0: case 4: case 0x2c: case 0x2d: return 4; case 1: return 3; case 0x4d: return 0.5f;
                 case 2: case 3: case 5: case 6: case 0x2e: case 0x2f: return 1; default: return 0; }
}
static UINT_PTR ex_texture(char *mat) {
    if (!readable(mat, 0x20) || !is_a(mat, "eMaterial")) return 0;
    char **arr = *(char ***)(mat + 0x10);
    if (!readable(arr, 4)) return 0;
    char *tex = arr[0];
    if (!readable(tex, 0x20)) return 0;
    char *bmp = *(char **)(tex + 0x10);
    if (!readable(bmp, 0x30) || !is_a(bmp, "eBitmap")) return 0;
    if (ex_seen((UINT_PTR)bmp)) return (UINT_PTR)bmp;
    unsigned w = *(unsigned *)(bmp + 8), h = *(unsigned *)(bmp + 0xc), vw = *(unsigned *)(bmp + 0x10),
             vh = *(unsigned *)(bmp + 0x14), fmt = *(unsigned *)(bmp + 0x24);
    size_t len = (size_t)(vw * vh * bpp_of(fmt));
    if (vw > 4096 || vh > 4096) len = 0;
    exw("T", 1); exu((unsigned)(UINT_PTR)bmp); exu(w); exu(h); exu(vw); exu(vh); exu(fmt);
    ex_blob(*(void **)(bmp + 0x1c), len);
    ex_blob(*(void **)(bmp + 0x20), *(void **)(bmp + 0x20) ? 0x400 : 0);
    exs(estr(*(char **)(bmp + 0x28)));
    g_extex++;
    return (UINT_PTR)bmp;
}
static void ex_arr(char *a, int el) {                      /* eGeoArray: +8 data, +0xc count */
    if (!readable(a, 0x10)) { exu(0); exu(0); return; }
    unsigned n = *(unsigned *)(a + 0xc);
    if (n > 1000000) n = 0;
    exu(n); ex_blob(*(void **)(a + 8), (size_t)n * el);
}
static void ex_geoset(char *gs) {
    if (ex_seen((UINT_PTR)gs)) return;
    exw("G", 1); exu((unsigned)(UINT_PTR)gs); exu(*(unsigned *)(gs + 0xc));
    ex_arr(*(char **)(gs + 0x14), 2);   /* pasy */
    ex_arr(*(char **)(gs + 0x18), 2);   /* indeksy */
    ex_arr(*(char **)(gs + 0x1c), 16);  /* positions */
    ex_arr(*(char **)(gs + 0x24), 16);  /* normalne */
    ex_arr(*(char **)(gs + 0x2c), 8);   /* UV */
    g_exgeo++;
}
static char g_chain[2048];
static void ex_walk(char *n, int depth, float *world, int *budget) {
    if (!readable(n, 0x48) || depth > 48 || --*budget < 0) return;
    const char *cls = rtti_name(n);
    size_t L = strlen(g_chain);
    char seg[256];
    if (is_a(n, "eProxy") && readable(n + 0xb0, 4))
        snprintf(seg, sizeof seg, "/%s:%s|%s", cls + 4, node_name(n), estr(*(char **)(n + 0xac)));
    else snprintf(seg, sizeof seg, "/%s:%s", cls + 4, node_name(n));
    if (L + strlen(seg) < sizeof g_chain - 1) strcat(g_chain, seg);
    float w[8]; memcpy(w, world, sizeof w);
    if (is_a(n, "eTransform") && readable(n + 0x88, 4)) memcpy(w, n + 0x68, 32);
    if (is_a(n, "eTriMesh") && readable(n + 0x5c, 4)) {
        char *gs = *(char **)(n + 0x58);
        if (readable(gs, 0x40)) {
            UINT_PTR tex = ex_texture(*(char **)(n + 0x3c));
            ex_geoset(gs);
            exw("M", 1); exs(g_chain); exu((unsigned)(UINT_PTR)gs); exu((unsigned)tex);
            exw(w, 32); exw(n + 0x40, 24); exu(*(unsigned *)(n + 0x1c));
            char *mat = *(char **)(n + 0x3c);
            if (readable(mat, 0x40)) exw(mat, 0x40); else { char z[0x40] = {0}; exw(z, 0x40); }
            g_exmesh++;
        }
    }
    if (is_a(n, "eGroup")) {
        int cnt = *(int *)(n + 0x3c);
        char **ch = *(char ***)(n + 0x44);
        if (cnt > 0 && cnt < 20000 && readable(ch, cnt * 4))
            for (int i = 0; i < cnt; i++) ex_walk(ch[i], depth + 1, w, budget);
    }
    g_chain[L] = 0;
}
static void export_scene(const char *lvl) {
    void **appp = (void **)VA(ADDR_APP);
    if (!readable(appp, 4) || !*appp) return;
    char *scene = *(char **)((char *)*appp + 0xc);
    CreateDirectoryA("kaoforge_export", NULL);
    char path[256]; snprintf(path, sizeof path, "kaoforge_export/%s.bin", lvl);
    g_ex = fopen(path, "wb");
    if (!g_ex) { logf_("export: cannot open %s\n", path); return; }
    exw("KEXP", 4); exu(1);
    g_exnseen = g_exmesh = g_extex = g_exgeo = 0; g_chain[0] = 0;
    float ident[8] = { 0, 0, 0, 1, 0, 0, 0, 1 };
    int budget = 300000;
    ex_walk(scene, 0, ident, &budget);
    fclose(g_ex);
    logf_("export %s: %d meshes, %d geosets, %d textures\n", lvl, g_exmesh, g_exgeo, g_extex);
}

/* automation: all levels in turn */
static int g_auto = -1, g_auto_state; static DWORD g_auto_t; static char *g_auto_scene;
static void export_all_start(void) { g_auto = 0; g_auto_state = 0; logf_("export: starting all levels\n"); }
static void export_tick(void) {
    if (g_auto < 0) return;
    int n = *(int *)VA(ADDR_LVL_COUNT);
    DWORD now = GetTickCount();
    void **appp = (void **)VA(ADDR_APP);
    char *scene = (readable(appp, 4) && *appp) ? *(char **)((char *)*appp + 0xc) : NULL;
    if (g_auto >= n || g_auto >= 64) { logf_("export: done\n"); g_auto = -1; return; }
    const char *nm = level_name(g_auto);
    if (!nm || !strcmp(nm, GRID_LEVEL)) { g_auto++; return; }
    char pb[256];
    snprintf(pb, sizeof pb, "kaoforge_export/%s.bin", nm);
    if (g_auto_state == 0 && GetFileAttributesA(pb) != INVALID_FILE_ATTRIBUTES) { g_auto++; return; }   /* already exported */
    if (g_auto_state == 0) {                   /* load */
        g_auto_scene = scene; g_auto_t = now; g_auto_state = 1;
        load_level(nm);
    } else if (g_auto_state == 1) {
        /* a new scene may live at the same address as the old one -> we do not compare pointers.
         * Wait 3 s for loading to start, then until the scene and the hero exist; after 25 s export anyway. */
        if (now - g_auto_t > 3000 && scene && (get_hero() || now - g_auto_t > 25000)) { g_auto_state = 2; g_auto_t = now; }
    } else if (g_auto_state == 2 && now - g_auto_t > 4000) {
        export_scene(nm);
        g_auto++; g_auto_state = 0;
    }
}

#include "modloader.c"

/* ---- settings (kaoforge.ini next to the exe) ---- */
static void settings_save(void) {
    FILE *f = fopen("kaoforge.ini", "w");
    if (!f) return;
    fprintf(f, "mouse=%d\nsens=%.3f\ninvx=%d\ninvy=%d\nsoft=%d\npp=%d\nfree=%d\ndist=%.0f\nspeed=%.3f\nflyspd=%.3f\njumph=%.3f\n",
            g_mouse_on, g_msens, g_mouse_invx, g_mouse_inv, g_soft_on, g_gfx_on, g_freecam, g_fc_dist, g_speed, g_flyspd, g_jumph);
    fclose(f);
}
static void settings_load(void) {
    FILE *f = fopen("kaoforge.ini", "r");
    if (!f) return;
    char k[32]; float v;
    while (fscanf(f, " %31[^=]=%f", k, &v) == 2) {
        if (!strcmp(k, "mouse")) g_mouse_on = v != 0;
        else if (!strcmp(k, "sens") && v > 0.01f && v < 100) g_msens = v;
        else if (!strcmp(k, "invx")) g_mouse_invx = v != 0;
        else if (!strcmp(k, "invy")) g_mouse_inv = v != 0;
        else if (!strcmp(k, "soft")) g_soft_on = v != 0;
        else if (!strcmp(k, "pp")) g_gfx_on = v != 0;
        else if (!strcmp(k, "free")) g_freecam = v != 0;
        else if (!strcmp(k, "speed") && v >= SPEED_MIN && v <= SPEED_MAX) g_speed = v;
        else if (!strcmp(k, "flyspd") && v >= SPEED_MIN && v <= SPEED_MAX) g_flyspd = v;
        else if (!strcmp(k, "jumph") && v >= 0.25f && v <= 6.0f) g_jumph = v;
        else if (!strcmp(k, "dist") && v >= 120 && v <= 3000) { g_fc_dist = v; g_dist_saved = 1; }
    }
    fclose(f);
    logf_("settings: mouse=%d sensitivity=%.2f invx=%d invy=%d soft=%d pp=%d\n",
          g_mouse_on, g_msens, g_mouse_invx, g_mouse_inv, g_soft_on, g_gfx_on);
}

/* ---- menu (Home): drawn with GL at the end of the frame, clicked with the mouse ---- */
static GLuint g_font_base;                 /* listy GL z wglUseFontBitmaps (znaki 32..127) */
static int g_font_state;                    /* 0 = not tried, 1 = ok, -1 = unavailable */
static int font_ready(void) {
    if (g_font_state) return g_font_state > 0;
    g_font_state = -1;
    HDC dc = wglGetCurrentDC();
    if (!dc) return 0;
    HFONT f = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    HGDIOBJ old = f ? SelectObject(dc, f) : NULL;
    GLuint base = glGenLists(96);
    if (base && wglUseFontBitmapsA(dc, 32, 96, base)) { g_font_base = base; g_font_state = 1; }
    if (old) SelectObject(dc, old);
    if (f) DeleteObject(f);
    logf_("menu: font %s\n", g_font_state > 0 ? "OK" : "MISSING");
    return g_font_state > 0;
}
enum { MI_MOUSE, MI_FREE, MI_DIST, MI_LMB, MI_SENS, MI_INVX, MI_INVY, MI_FLY, MI_FAST, MI_SPEED, MI_FLYSPD, MI_JUMPH, MI_JUMP, MI_GOD, MI_SOFT, MI_PP, MI_LSEL, MI_GRID, MI_L05, MI_EXPORT, MI_EXPHERE, MI_CLOSE, MI_N };
#define MENU_X 60
#define MENU_Y 60
#define MENU_W 560
#define ROW_H  34

static void menu_toggle(void) {
    g_menu ^= 1;
    g_prevkey[VK_LBUTTON] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!g_menu) settings_save();
    logf_("menu=%d\n", g_menu);
}

static const char *onoff(int v) { return v ? "ON" : "off"; }

static int g_lsel = -1;                     /* level selected in the menu (levels.ini index) */
static const char *lsel_name(void) {
    int n = *(int *)VA(ADDR_LVL_COUNT);
    if (g_lsel < 0) { g_lsel = 0; for (int i = 0; i < n && i < 64; i++) { const char *m = level_name(i); if (m && !strcmp(m, GRID_LEVEL)) g_lsel = i; } }
    if (g_lsel >= n) g_lsel = 0;
    const char *m = level_name(g_lsel);
    return m ? m : "?";
}
static void menu_label(int i, char *b, size_t n) {
    if (i >= MI_N) { snprintf(b, n, "%s", g_flabel[i - MI_N]()); return; }
    switch (i) {
    case MI_MOUSE: snprintf(b, n, "Mouse camera: %s", onoff(g_mouse_on)); break;
    case MI_LMB:   snprintf(b, n, "Punch on left mouse button: %s", onoff(g_lmb_punch)); break;
    case MI_FREE:  snprintf(b, n, "Free camera (no lag): %s", onoff(g_freecam)); break;
    case MI_DIST:  snprintf(b, n, "Camera distance:   <  %.0f  >", g_fc_dist); break;
    case MI_SENS:  snprintf(b, n, "Mouse sensitivity:   <  %.2f  >", g_msens); break;
    case MI_INVX:  snprintf(b, n, "Invert X axis (sideways): %s", onoff(g_mouse_invx)); break;
    case MI_INVY:  snprintf(b, n, "Invert Y axis (up/down): %s", onoff(g_mouse_inv)); break;
    case MI_FLY:   snprintf(b, n, "Fly in the look direction: %s", onoff(g_fly)); break;
    case MI_FAST:  snprintf(b, n, "Fast movement x3: %s", onoff(g_fast)); break;
    case MI_SPEED: snprintf(b, n, "Movement speed  %.2fx", g_speed); break;
    case MI_FLYSPD: snprintf(b, n, "Flight speed  %.2fx", g_flyspd); break;
    case MI_JUMPH: snprintf(b, n, "Jump height  %.2fx", g_jumph); break;
    case MI_JUMP:  snprintf(b, n, "High jump: %s", onoff(g_jump)); break;
    case MI_GOD:   snprintf(b, n, "God mode: %s", onoff(g_god)); break;
    case MI_SOFT:  snprintf(b, n, "Soft shadows: %s", onoff(g_soft_on)); break;
    case MI_PP:    snprintf(b, n, "Anti-aliasing (FXAA): %s", onoff(g_gfx_on)); break;
    case MI_LSEL:  snprintf(b, n, "Level:  <  %s  >", lsel_name()); break;
    case MI_GRID:  snprintf(b, n, "> Load the selected level"); break;
    case MI_L05:   snprintf(b, n, "> Load level 05 (Big Trees)"); break;
    case MI_EXPORT:  snprintf(b, n, g_auto >= 0 ? "> Export running... (level %d)" : "> Export props from ALL levels", g_auto); break;
    case MI_EXPHERE: snprintf(b, n, "> Export the current level only"); break;
    default:       snprintf(b, n, "Close  [Home]"); break;
    }
}

static void menu_click(int i, int right_half) {
    if (i >= MI_N) { if (g_fclick[i - MI_N]) g_fclick[i - MI_N](right_half); return; }
    switch (i) {
    case MI_MOUSE: g_mouse_on ^= 1; break;
    case MI_LMB:   g_lmb_punch ^= 1; break;
    case MI_FREE:  g_freecam ^= 1; g_fc_init = 0; break;
    case MI_DIST:  g_dist_saved = 1; if (right_half) g_fc_dist *= 1.15f; else g_fc_dist /= 1.15f;
                   if (g_fc_dist < 120) g_fc_dist = 120; if (g_fc_dist > 3000) g_fc_dist = 3000; break;
    case MI_SENS:  if (right_half) g_msens *= 1.15f; else g_msens /= 1.15f; break;
    case MI_INVX:  g_mouse_invx ^= 1; break;
    case MI_INVY:  g_mouse_inv ^= 1; break;
    case MI_FLY:   g_fly ^= 1; break;
    case MI_FAST:  g_fast ^= 1; break;
    case MI_SPEED: case MI_FLYSPD: case MI_JUMPH: return;   /* sliders: dragging is handled in menu_frame */
    case MI_JUMP:  g_jump ^= 1; break;
    case MI_GOD:   g_god ^= 1; break;
    case MI_SOFT:  g_soft_on ^= 1; break;
    case MI_PP:    g_gfx_on ^= 1; break;
    case MI_LSEL: { int n = *(int *)VA(ADDR_LVL_COUNT); lsel_name(); if (n > 0) g_lsel = (g_lsel + (right_half ? 1 : n - 1)) % n; return; }
    case MI_GRID:  { char nm[64]; snprintf(nm, sizeof nm, "%s", lsel_name()); menu_toggle(); load_level(nm); return; }
    case MI_L05:   menu_toggle(); load_level("05_wielkie_drzewa"); return;
    case MI_EXPORT:  if (g_auto < 0) { menu_toggle(); export_all_start(); } return;
    case MI_EXPHERE: export_scene("biezacy"); return;
    default:       menu_toggle(); return;
    }
    settings_save();
}

static void text_at(float x, float y, const char *s) {
    glRasterPos2f(x, y);
    glListBase(g_font_base - 32);
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c >= 32 && c < 128) glCallLists(1, GL_UNSIGNED_BYTE, &c); }
}

static void rect(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS); glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1); glEnd();
}

static void hud_export(void) {
    if (g_auto < 0 || g_menu) return;
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS); glPushAttrib(GL_LIST_BIT);
    if (p_UseProgram) p_UseProgram(0);
    int hasfont = font_ready();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, vp[2], vp[3], 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor4f(0, 0, 0, 0.7f); rect(20, 20, 820, 64);
    if (hasfont) {
        char b[160]; const char *nm = level_name(g_auto);
        snprintf(b, sizeof b, "EXPORT: level %d/%d  %s  (%s)", g_auto + 1, *(int *)VA(ADDR_LVL_COUNT), nm ? nm : "?",
                 g_auto_state == 1 ? "wczytywanie" : g_auto_state == 2 ? "czekam" : "start");
        glColor4f(1.0f, 0.8f, 0.3f, 1); text_at(32, 50, b);
    }
    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib(); glPopAttrib();
}

typedef struct { int row; float *val; float mn, mx; } Slider;
static Slider g_sl[] = { { MI_SPEED, &g_speed, SPEED_MIN, SPEED_MAX }, { MI_FLYSPD, &g_flyspd, SPEED_MIN, SPEED_MAX }, { MI_JUMPH, &g_jumph, 0.25f, 6.0f } };
#define N_SLIDERS 3
static void menu_frame(void) {
    hud_export();
    if (!g_menu) return;
    HWND hw = readable((void *)VA(ADDR_GAME_HWND), 4) ? *(HWND *)VA(ADDR_GAME_HWND) : NULL;
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    int W = vp[2], H = vp[3];
    if (W <= 0 || H <= 0) return;
    /* kursor w pikselach bufora */
    float mx = -1, my = -1;
    if (hw) {
        POINT p; RECT rc; GetCursorPos(&p); ScreenToClient(hw, &p); GetClientRect(hw, &rc);
        if (rc.right > 0 && rc.bottom > 0) { mx = p.x * (float)W / rc.right; my = p.y * (float)H / rc.bottom; }
    }
    int hover = -1;
    for (int i = 0; i < MI_N + g_nfmenu; i++) {
        float y0 = MENU_Y + 50 + i * ROW_H;
        if (mx >= MENU_X && mx <= MENU_X + MENU_W && my >= y0 && my < y0 + ROW_H) hover = i;
    }
    {   /* sliders (movement speed, flight speed, jump height): drag along the track, logarithmic scale, snap to 1.00x */
        static int drag = -1; float tx0 = MENU_X + 330, tx1 = MENU_X + MENU_W - 24;
        int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!lmb) { if (drag >= 0) settings_save(); drag = -1; }
        for (int k = 0; k < N_SLIDERS; k++) {
            float y0 = MENU_Y + 50 + g_sl[k].row * ROW_H;
            if (!lmb || drag >= 0 || g_prevkey[VK_LBUTTON]) continue;
            if (mx >= tx0 - 8 && mx <= tx1 + 8 && my >= y0 && my < y0 + ROW_H) drag = k;
        }
        if (drag >= 0) {
            Slider *sl = &g_sl[drag];
            float t = (mx - tx0) / (tx1 - tx0); if (t < 0) t = 0; if (t > 1) t = 1;
            *sl->val = sl->mn * powf(sl->mx / sl->mn, t);
            if (fabsf(*sl->val - 1.0f) < 0.06f) *sl->val = 1.0f;
            g_prevkey[VK_LBUTTON] = 1; hover = -1;                           /* the click does not fall through to menu_click */
        }
    }
    if (pressed(VK_LBUTTON) && hover >= 0) menu_click(hover, mx > MENU_X + MENU_W / 2);
    if (!g_menu) return;

    GLint bound; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushAttrib(GL_LIST_BIT);
    if (p_UseProgram) p_UseProgram(0);
    int hasfont = font_ready();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); glColorMask(1, 1, 1, 1);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();

    float bottom = MENU_Y + 50 + (MI_N + g_nfmenu) * ROW_H + 40;
    glColor4f(0.05f, 0.06f, 0.09f, 0.82f); rect(MENU_X - 10, MENU_Y, MENU_X + MENU_W + 10, bottom);
    glColor4f(1.0f, 0.62f, 0.15f, 1.0f);   rect(MENU_X - 10, MENU_Y, MENU_X + MENU_W + 10, MENU_Y + 4);
    if (hasfont) { glColor4f(1.0f, 0.75f, 0.3f, 1); text_at(MENU_X + 10, MENU_Y + 34, "KAOFORGE - settings"); }
    for (int i = 0; i < MI_N + g_nfmenu; i++) {
        float y0 = MENU_Y + 50 + i * ROW_H;
        if (i == hover) { glColor4f(1.0f, 0.62f, 0.15f, 0.25f); rect(MENU_X, y0, MENU_X + MENU_W, y0 + ROW_H - 2); }
        if (i == MI_LSEL || i == MI_CLOSE || i == MI_N) { glColor4f(1, 1, 1, 0.12f); rect(MENU_X, y0 - 2, MENU_X + MENU_W, y0 - 1); }
        if (!hasfont) continue;
        char b[96]; menu_label(i, b, sizeof b);
        int on = strstr(b, ": ON") != NULL;
        if (on) glColor4f(0.55f, 1.0f, 0.55f, 1); else glColor4f(0.92f, 0.92f, 0.95f, 1);
        text_at(MENU_X + 10, y0 + 23, b);
    }
    for (int k = 0; k < N_SLIDERS; k++) {   /* slider tracks */
        Slider *sl = &g_sl[k];
        float tx0 = MENU_X + 330, tx1 = MENU_X + MENU_W - 24, cy = MENU_Y + 50 + sl->row * ROW_H + ROW_H * 0.5f - 1;
        float span = logf(sl->mx / sl->mn);
        float t = logf(*sl->val / sl->mn) / span, x1 = tx0 + (tx1 - tx0) * t;
        float xm = tx0 + (tx1 - tx0) * (logf(1.0f / sl->mn) / span);
        glColor4f(1, 1, 1, 0.22f); rect(tx0, cy - 3, tx1, cy + 3);
        glColor4f(1.0f, 0.62f, 0.15f, 0.95f); rect(tx0, cy - 3, x1, cy + 3);
        glColor4f(1, 1, 1, 0.6f); rect(xm - 1, cy - 9, xm + 1, cy + 9);              /* znacznik 1.00x */
        glColor4f(1, 1, 1, 1); rect(x1 - 6, cy - 10, x1 + 6, cy + 10);
    }
    if (hasfont) { glColor4f(0.7f, 0.7f, 0.75f, 1); text_at(MENU_X + 10, bottom - 14, "Click = toggle; arrows: left/right half of the row"); }
    if (mx >= 0) {                                   /* own cursor (the game may hide the system one) */
        glColor4f(1, 1, 1, 1);
        glBegin(GL_TRIANGLES); glVertex2f(mx, my); glVertex2f(mx, my + 22); glVertex2f(mx + 14, my + 16); glEnd();
        glColor4f(0, 0, 0, 1);
        glBegin(GL_LINE_LOOP); glVertex2f(mx, my); glVertex2f(mx, my + 22); glVertex2f(mx + 14, my + 16); glEnd();
    }
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib();
    glPopAttrib();
    glBindTexture(GL_TEXTURE_2D, bound);
}


/* ===== fly in the look direction =====
 * Kao's physics (gravity, ground snapping) lives in ePhysics::update (FUN_004cb9a0, thiscall(this, ctx), this[2] = actor,
 * dt = *(float*)(ctx+0x1c)). In fly mode we replace that function for the hero: instead of physics we integrate the position ourselves
 * (hero+0x58, copies +0x78/+0x98).
 * Controls: the game's left stick (LY: +1 = forward, LX: +1 = right; from the keyboard WASD per the game's key bindings) carries Kao along the
 * camera's VIEW DIRECTION (from P*V: f_camera) - look up = climb at an angle. PageUp/PageDown = vertical. No input: hover (no gravity).
 * No collisions while flying (physics is skipped). Model rotation: yaw follows the flight direction, offset calibrated from walking. */
#define FLY_BASE 1200.0f
static void (__attribute__((thiscall)) *orig_phys)(int *self, void *ctx);
static float g_fvel[3];
static float g_yawc, g_yaws; static int g_yawn; static float g_yawoff; static int g_yawok;

static void fly_step(float dt) {
    char *hero = (char *)get_hero(); if (!hero) return;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
    float *p = (float *)(hero + OFF_LPOS), lx = 0, ly = 0;
    void *pad = (void *)VA(ADDR_PAD0);
    if (orig_getLX) lx = orig_getLX(pad);
    if (orig_getLY) ly = orig_getLY(pad);
    if (forge_input_blocked()) lx = ly = 0;
    if (fabsf(lx) < 0.15f) lx = 0; if (fabsf(ly) < 0.15f) ly = 0;
    float eye[3], fwd[3] = { 1, 0, 0 };
    if (!f_camera(eye, fwd)) { fwd[0] = 1; fwd[1] = 0; fwd[2] = 0; }
    float hl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]); if (hl < 1e-3f) hl = 1;
    float rx = fwd[1] / hl, ry = -fwd[0] / hl;                     /* right = fwd x up */
    float tv[3] = { fwd[0] * ly + rx * lx, fwd[1] * ly + ry * lx, fwd[2] * ly };
    int in = !forge_input_blocked();
    if (in && (GetAsyncKeyState(VK_PRIOR) & 0x8000)) tv[2] += 1.0f;
    if (in && (GetAsyncKeyState(VK_NEXT) & 0x8000))  tv[2] -= 1.0f;
    float tl = sqrtf(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
    if (tl > 1.0f) { tv[0] /= tl; tv[1] /= tl; tv[2] /= tl; }
    float sp = FLY_BASE * (g_flyspd < SPEED_MIN ? SPEED_MIN : g_flyspd > SPEED_MAX ? SPEED_MAX : g_flyspd);
    float k = 1.0f - expf(-6.0f * dt);                              /* smooth acceleration/braking */
    for (int i = 0; i < 3; i++) { g_fvel[i] += (tv[i] * sp - g_fvel[i]) * k; p[i] += g_fvel[i] * dt; }
    float *w = (float *)(hero + OFF_WPOS), *w2 = (float *)(hero + 0x98);
    for (int i = 0; i < 3; i++) { w[i] = p[i]; w2[i] = p[i]; }
    if (g_yawok && g_fvel[0] * g_fvel[0] + g_fvel[1] * g_fvel[1] > 80.0f * 80.0f) {     /* model patrzy w kierunku lotu */
        float yaw = atan2f(g_fvel[1], g_fvel[0]) + g_yawoff;
        float *q = (float *)(hero + 0x48); q[0] = 0; q[1] = 0; q[2] = sinf(yaw * 0.5f); q[3] = cosf(yaw * 0.5f);
    }
}
static void __attribute__((thiscall)) det_phys(int *self, void *ctx) {
    if (g_fly && self[2] && (char *)self[2] == (char *)get_hero()) { fly_step(*(float *)((char *)ctx + 0x1c)); return; }
    orig_phys(self, ctx);
}
static void fly_calibrate(float dx, float dy, float dt, char *hero) {   /* walking: model rotation relative to the movement direction */
    if (g_fly || dt <= 0 || sqrtf(dx * dx + dy * dy) / dt < 250.0f) return;
    float *q = (float *)(hero + 0x48);
    if (fabsf(q[0]) > 0.05f || fabsf(q[1]) > 0.05f) return;
    float d = 2.0f * atan2f(q[2], q[3]) - atan2f(dy, dx);
    g_yawc += cosf(d); g_yaws += sinf(d); g_yawn++;
    if (g_yawn >= 12) { g_yawoff = atan2f(g_yaws, g_yawc); if (!g_yawok) logf_("fly: offset obrotu modelu = %.2f rad\n", g_yawoff); g_yawok = 1; }
    if (g_yawn > 400) { g_yawc *= 0.5f; g_yaws *= 0.5f; g_yawn = 200; }
}

static void tick(void) {
    DWORD now = GetTickCount();
    float dt = g_lastTick ? (now - g_lastTick) / 1000.0f : 0.016f;
    g_lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    do_patches();   /* in case glutInit did not go through the hook */
    {   /* hook of Kao's physics (flying) - once; built-in (not removed by a mod reload) */
        static int hooked;
        if (!hooked) { hooked = 1; g_forge_builtin_phase = 1;
            int r = f_hook((void *)VA(0x004CB9A0u), (void *)det_phys, (void **)&orig_phys); g_forge_builtin_phase = 0;
            logf_("fly: hook ePhysics::update %s\n", r == 0 ? "OK" : "FAILED (flying unavailable)"); }
        static int prev_fly; if (g_fly != prev_fly) { prev_fly = g_fly; g_fvel[0] = g_fvel[1] = g_fvel[2] = 0; g_haveLast = 0; }
    }

    if (pressed(VK_F1)) { g_fly  ^= 1; logf_("fly=%d\n", g_fly); }
    if (pressed(VK_F2)) { g_fast ^= 1; logf_("fast=%d\n", g_fast); }
    if (g_fast != g_fast_prev) g_speed = g_fast ? (fabsf(g_speed - 1.0f) > 0.01f ? g_speed : 3.0f) : 1.0f;   /* F2 / the fast command = toggle 1x <-> 3x */
    g_fast = g_fast_prev = fabsf(g_speed - 1.0f) > 0.01f;
    if (pressed(VK_F3)) { g_jump ^= 1; logf_("jump=%d\n", g_jump); }
    if (g_jump != g_jump_prev) g_jumph = g_jump ? (fabsf(g_jumph - 1.0f) > 0.01f ? g_jumph : 1.6f) : 1.0f;   /* F3 / the jump command = toggle 1x <-> 1.6x */
    g_jump = g_jump_prev = fabsf(g_jumph - 1.0f) > 0.01f;
    if (pressed(VK_F4)) { g_god  ^= 1; logf_("god=%d\n", g_god); }
    if (pressed(VK_F6)) load_level(GRID_LEVEL);
    if (pressed(VK_F5)) load_level("05_wielkie_drzewa");
    if (pressed(VK_F7)) dump_scene();
    if (pressed(VK_F8)) { g_gfx_on ^= 1; settings_save(); logf_("gfx post-processing=%d (textures filtered: %d)\n", g_gfx_on, g_texhooks); }
    if (pressed(VK_F11)) { g_soft_on ^= 1; settings_save(); logf_("gfx soft shadows=%d (frames with shadow: %d)\n", g_soft_on, g_shadow_frames); }
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int f12 = pressed(VK_F12);
    if (pressed(VK_HOME)) menu_toggle();
    export_tick();
    if (g_frame - g_mframe > 3) mouse_release();       /* menu/loading: the camera does not poll the stick */
    {   /* mouse diagnostics every 2 s while it moves */
        static DWORD lastm;
        if ((g_mx != 0 || g_my != 0) && now - lastm > 2000) { lastm = now; logf_("mysz: rx=%.2f ry=%.2f\n", g_mx, g_my); }
    }

    if (g_god && readable((void *)VA(ADDR_UNL_ENERGY), 1))
        *(volatile BYTE *)VA(ADDR_UNL_ENERGY) = 1;

    char *hero = (char *)get_hero();
    if (!hero) { g_haveLast = 0; return; }
    if (pressed(VK_F9)) dump_hero(hero);
    barrels_tick(hero);

    float *p = (float *)(hero + OFF_LPOS);
    if (f12 && shift) scan_pos(p);

    {   /* hero position every 2 s (first 60 entries; the counter resets after F6) */
        static DWORD lastpos; static int npos;
        if (now - lastpos > 2000 && npos < 60) {
            lastpos = now; npos++;
            logf_("pos: %.1f %.1f %.1f\n", p[0], p[1], p[2]);
        }
        if (GetAsyncKeyState(VK_F6) & 0x8000) npos = 0;
    }

    if (g_haveLast) {
        float dx = p[0] - g_last[0], dy = p[1] - g_last[1], dz = p[2] - g_last[2];
        if (dx*dx + dy*dy + dz*dz < 10000.0f) {          /* ignoruj teleporty */
            fly_calibrate(dx, dy, dt, hero);
            if (g_fast && !g_fly) { p[0] += dx * (g_speed - 1.0f); p[1] += dy * (g_speed - 1.0f); }
            if (g_jump && !g_fly && dz > 0.0f) p[2] += dz * (g_jumph - 1.0f);
        }
    }
    /* keep the derived copy consistent within this frame (render/camera) */
    float *w = (float *)(hero + OFF_WPOS);
    w[0] = p[0]; w[1] = p[1]; w[2] = p[2];

    g_last[0] = p[0]; g_last[1] = p[1]; g_last[2] = p[2];
    g_haveLast = 1;
}

__declspec(dllexport) void Hook_glutSwapBuffers(void) {
    if (g_unsupported) { if (real_SwapBuffers) real_SwapBuffers(); return; }
    g_frame++;
    tick();
    {   /* Kao2 Forge: mody z mods/ */
        static LARGE_INTEGER last; LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        float fdt = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) / fq.QuadPart) : 0.016f;
        last = now; if (fdt > 0.1f) fdt = 0.1f;
        forge_load_mods();
        forge_tick(fdt);
    }
    gfx_frame();
    forge_draw();
    menu_frame();
    if (real_SwapBuffers) real_SwapBuffers();
}

/* ---- crash catcher: the first exception in game code -> log (EIP, address, registers, stack) ---- */
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *e) {
    static int n;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        c != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *x = e->ContextRecord;
    if (x->Eip < VA(0x401000u) || x->Eip >= VA(0x960000u)) return EXCEPTION_CONTINUE_SEARCH;  /* game code only (IsBadReadPtr etc. are ignored) */
    if (n++ > 4) return EXCEPTION_CONTINUE_SEARCH;
    logf_("CRASH %08lx eip=%08lx adr=%08lx (%s) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
          c, x->Eip, (unsigned long)e->ExceptionRecord->ExceptionInformation[1],
          e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
          x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp);
    DWORD *sp = (DWORD *)x->Esp;
    logf_("stos:");
    for (int i = 0; i < 48; i++) {                    /* return addresses into the exe */
        if (!readable(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= VA(0x401000u) && v < VA(0x960000u)) logf_(" %08lx", v);
    }
    logf_("\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_delta = (UINT_PTR)GetModuleHandleA(NULL) - 0x00400000u;
        g_log = fopen("kaoforge.log", "w");
        g_real = LoadLibraryA("glut32_real.dll");
        if (g_real) {
            real_SwapBuffers = (void (*)(void))GetProcAddress(g_real, "glutSwapBuffers");
            g_real_glutInit = (void *)GetProcAddress(g_real, "glutInit");
        }
        {   /* identify the game build by exe size (the loader's addresses only apply to that build) */
            char exe[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA fa; GetModuleFileNameA(NULL, exe, MAX_PATH);
            DWORD sz = GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) ? fa.nFileSizeLow : 0;
            g_unsupported = sz != KNOWN_EXE_SIZE;
            logf_("exe: %s (%lu bytes)%s\n", exe, (unsigned long)sz, g_unsupported ? " - UNSUPPORTED BUILD" : "");
        }
        logf_("KaoForge 0.1.0 (API %d) loaded, delta=%x real=%p swap=%p init=%p\n", KAOAPI_VERSION, (unsigned)g_delta,
              (void *)g_real, (void *)real_SwapBuffers, g_real_glutInit);
        settings_load();
        AddVectoredExceptionHandler(1, crash_handler);
    }
    return TRUE;
}
