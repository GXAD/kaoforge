/*
 * modapi.c - KaoForge API: hooks, calling game functions, scene, events, console, configuration, hot-reload.
 * Included from modloader.c (before the g_api table). Everything runs on the game thread.
 */
#include <math.h>
static void rect(float x0, float y0, float x1, float y1);

/* ===================== patches with undo ===================== */
#define FPATCH_MAX 512
typedef struct { BYTE *addr; BYTE orig[24]; int n; int builtin; } FPatch;
static FPatch g_fp[FPATCH_MAX]; static int g_nfp;
static int g_forge_builtin_phase;          /* 1 = built-in registrations (not removed by reload) */

static int mem_write(void *dst, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return -1;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 0;
}
static int fp_save(void *addr, int n) {
    if (g_nfp >= FPATCH_MAX || n > 24 || !readable(addr, n)) return -1;
    FPatch *p = &g_fp[g_nfp++];
    p->addr = (BYTE *)addr; p->n = n; p->builtin = g_forge_builtin_phase;
    memcpy(p->orig, addr, n);
    return 0;
}
static void fp_restore_mods(void) {                    /* from the end: later patches may overlap earlier ones */
    for (int i = g_nfp - 1; i >= 0; i--)
        if (!g_fp[i].builtin) { mem_write(g_fp[i].addr, g_fp[i].orig, g_fp[i].n); g_fp[i] = g_fp[--g_nfp]; }
}

/* ===================== tiny x86 length decoder (function prologues) ===================== */
static int modrm_len(const BYTE *p) {                  /* p -> bajt ModRM; zwraca ModRM + SIB + disp */
    BYTE m = p[0], mod = m >> 6, rm = m & 7; int n = 1;
    if (mod == 3) return 1;
    if (rm == 4) { n++; if (mod == 0 && (p[1] & 7) == 5) n += 4; }
    if (mod == 0 && rm == 5) n += 4;
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n;
}
static int insn_len(const BYTE *p) {
    int pre = 0;
    while (p[pre] == 0x64 || p[pre] == 0x65 || p[pre] == 0x66 || p[pre] == 0xF2 || p[pre] == 0xF3) pre++;
    const BYTE *q = p + pre; BYTE o = q[0];
    int imm16 = p[0] == 0x66 || (pre && p[pre - 1] == 0x66);
    if ((o >= 0x50 && o <= 0x5F) || o == 0x90 || o == 0xC3 || o == 0xCC || (o >= 0x40 && o <= 0x4F) || o == 0x99 || o == 0x9C || o == 0x9D)
        return pre + 1;
    if (o == 0x6A || o == 0xEB || (o >= 0x70 && o <= 0x7F) || o == 0xA8 || o == 0x04 || o == 0x0C || o == 0x24 || o == 0x2C || o == 0x34 || o == 0x3C)
        return pre + 2;
    if (o == 0x68 || o == 0xE8 || o == 0xE9 || o == 0xA9 || o == 0x05 || o == 0x0D || o == 0x25 || o == 0x2D || o == 0x35 || o == 0x3D)
        return pre + 1 + (imm16 ? 2 : 4);
    if (o >= 0xB8 && o <= 0xBF) return pre + 1 + (imm16 ? 2 : 4);
    if (o >= 0xB0 && o <= 0xB7) return pre + 2;
    if (o == 0xA1 || o == 0xA3 || o == 0xA0 || o == 0xA2) return pre + 5;
    if (o == 0xC2) return pre + 3;
    if (o == 0x0F) {
        BYTE o2 = q[1];
        if (o2 >= 0x80 && o2 <= 0x8F) return pre + 6;
        if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF || o2 == 0xAF || (o2 >= 0x40 && o2 <= 0x4F) || (o2 >= 0x90 && o2 <= 0x9F) ||
            o2 == 0x10 || o2 == 0x11 || o2 == 0x28 || o2 == 0x29 || o2 == 0x57 || o2 == 0x6F || o2 == 0x7F || o2 == 0xD6)
            return pre + 2 + modrm_len(q + 2);
        return -1;
    }
    switch (o) {                                       /* opcodes with ModRM and no immediate */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D: case 0x8F:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
    case 0xFE: case 0xFF:
        return pre + 1 + modrm_len(q + 1);
    case 0x80: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6: case 0x6B:
        return pre + 1 + modrm_len(q + 1) + 1;
    case 0x81: case 0xC7: case 0x69:
        return pre + 1 + modrm_len(q + 1) + (imm16 ? 2 : 4);
    case 0xF6: return pre + 1 + modrm_len(q + 1) + (((q[1] >> 3) & 7) < 2 ? 1 : 0);
    case 0xF7: return pre + 1 + modrm_len(q + 1) + (((q[1] >> 3) & 7) < 2 ? (imm16 ? 2 : 4) : 0);
    }
    return -1;
}

/* ===================== hooki ===================== */
static int f_hook(void *target, void *detour, void **orig) {
    BYTE *t = (BYTE *)target;
    if (!readable(t, 32) || !detour) return -1;
    int n = 0;
    while (n < 5) {
        int l = insn_len(t + n);
        if (l <= 0) { logf_("forge hook %p: nieznana instrukcja %02x %02x %02x (+%d)\n", target, t[n], t[n + 1], t[n + 2], n); return -2; }
        if ((t[n] >= 0x70 && t[n] <= 0x7F) || t[n] == 0xEB || (t[n] == 0x0F && t[n + 1] >= 0x80 && t[n + 1] <= 0x8F)) {
            logf_("forge hook %p: conditional jump in the prologue - cannot be relocated\n", target); return -3;
        }
        n += l;
    }
    BYTE *tr = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return -4;
    memcpy(tr, t, n);
    for (int k = 0; k < n; ) {                         /* relocate relative call/jmp rel32 */
        int l = insn_len(tr + k);
        if (tr[k] == 0xE8 || tr[k] == 0xE9) {
            INT32 rel = *(INT32 *)(tr + k + 1);
            BYTE *dst = t + k + 5 + rel;
            *(INT32 *)(tr + k + 1) = (INT32)(dst - (tr + k + 5));
        }
        k += l;
    }
    tr[n] = 0xE9; *(INT32 *)(tr + n + 1) = (INT32)((t + n) - (tr + n + 5));
    if (fp_save(t, n) < 0) return -5;
    BYTE j[24]; j[0] = 0xE9; *(INT32 *)(j + 1) = (INT32)((BYTE *)detour - (t + 5));
    for (int k = 5; k < n; k++) j[k] = 0x90;
    mem_write(t, j, n);
    if (orig) *orig = tr;
    logf_("forge: hook %p -> %p (%d B prologu)\n", target, detour, n);
    return 0;
}
static int f_hook_vtable(void **slot, void *fn, void **orig) {
    if (!readable(slot, 4) || fp_save(slot, 4) < 0) return -1;
    if (orig) *orig = *slot;
    return mem_write(slot, &fn, 4);
}
static int f_hook_call(void *site, void *fn, void **orig) {
    BYTE *p = (BYTE *)site;
    if (!readable(p, 5) || p[0] != 0xE8 || fp_save(p, 5) < 0) return -1;
    if (orig) *orig = p + 5 + *(INT32 *)(p + 1);
    INT32 rel = (INT32)((BYTE *)fn - (p + 5));
    return mem_write(p + 1, &rel, 4);
}
static int f_hook_import(const char *dll, const char *func, void *fn, void **orig) {
    /* remember the IAT slot so it can be undone: found by its value before the swap */
    HMODULE exe = GetModuleHandleA(NULL);
    void *cur = (void *)GetProcAddress(GetModuleHandleA(dll), func);
    void *o = iat_hook(exe, dll, func, fn);
    if (!o) return -1;
    if (orig) *orig = o;
    (void)cur;
    return 0;
}
static int f_patch(void *addr, const void *b, size_t n) {
    if (n > 24) {                                      /* longer: in chunks of 24 bytes */
        for (size_t k = 0; k < n; k += 24) if (f_patch((BYTE *)addr + k, (const BYTE *)b + k, n - k < 24 ? n - k : 24)) return -1;
        return 0;
    }
    if (fp_save(addr, (int)n) < 0) return -1;
    return mem_write(addr, b, n);
}
static int f_write(void *d, const void *s, size_t n) { return readable(d, 1) ? mem_write(d, s, n) : -1; }

/* ===================== calling game functions ===================== */
static unsigned f_call(void *fn, void *self, int argc, const unsigned *argv) {
    unsigned r;
    __asm__ volatile(
        "mov %%esp, %%esi\n\t"
        "1: test %%edx, %%edx\n\t"
        "jz 2f\n\t"
        "dec %%edx\n\t"
        "pushl (%%eax,%%edx,4)\n\t"
        "jmp 1b\n\t"
        "2: mov %%edi, %%ecx\n\t"
        "call *%%ebx\n\t"
        "mov %%esi, %%esp\n\t"
        : "=a"(r), "+d"(argc)                  /* EDX: loop counter and a volatile register of the callee */
        : "b"(fn), "D"(self), "0"(argv)
        : "ecx", "esi", "memory", "cc");
    return r;
}
static float g_fret;
static float f_call_f(void *fn, void *self, int argc, const unsigned *argv) {
    __asm__ volatile(
        "mov %%esp, %%esi\n\t"
        "1: test %%edx, %%edx\n\t"
        "jz 2f\n\t"
        "dec %%edx\n\t"
        "pushl (%%eax,%%edx,4)\n\t"
        "jmp 1b\n\t"
        "2: mov %%edi, %%ecx\n\t"
        "call *%%ebx\n\t"
        "mov %%esi, %%esp\n\t"
        "fstps %[out]\n\t"                   /* result from ST0 into a static variable (absolute address) */
        : [out] "=m"(g_fret), "+a"(argv), "+d"(argc)
        : "b"(fn), "D"(self)
        : "ecx", "esi", "memory", "cc");
    return g_fret;
}

/* ===================== scene and objects ===================== */
static void *f_scene(void);
static int walk_rec(char *n, int depth, KaoWalkFn cb, void *ctx, int *budget) {
    if (!readable(n, 0x48) || depth > 64 || --*budget < 0) return 0;
    int r = cb(n, depth, ctx);
    if (r == 1) return 1;
    if (r == 2 || !is_a(n, "eGroup")) return 0;
    int cnt = *(int *)(n + 0x3c); char **ch = *(char ***)(n + 0x44);
    if (cnt <= 0 || cnt > 50000 || !readable(ch, cnt * 4)) return 0;
    for (int i = 0; i < cnt; i++) if (walk_rec(ch[i], depth + 1, cb, ctx, budget)) return 1;
    return 0;
}
static void f_walk(void *root, KaoWalkFn cb, void *ctx) {
    int budget = 500000;
    if (!root) root = f_scene();
    if (root && cb) walk_rec((char *)root, 0, cb, ctx, &budget);
}
static int f_children(void *node, void ***list) {
    char *n = (char *)node;
    if (!is_a(n, "eGroup")) return 0;
    int cnt = *(int *)(n + 0x3c);
    if (cnt <= 0 || cnt > 50000 || !readable(*(void **)(n + 0x44), cnt * 4)) return 0;
    if (list) *list = *(void ***)(n + 0x44);
    return cnt;
}
static int f_node_pos(void *node, float *o) {
    char *n = (char *)node;
    if (is_a(n, "eTransform") && readable(n + 0x78, 12)) { memcpy(o, n + 0x78, 12); return 1; }
    if (readable(n + 0x20, 12)) { memcpy(o, n + 0x20, 12); return 1; }
    return 0;
}
static int f_node_set_pos(void *node, const float *p) {
    char *n = (char *)node;
    if (!is_a(n, "eTransform") || !readable(n + 0x84, 4)) return 0;
    memcpy(n + 0x58, p, 12); memcpy(n + 0x78, p, 12);
    return 1;
}
static void f_node_set_visible(void *node, int on) {
    char *n = (char *)node; if (!readable(n + 0x20, 4)) return;
    if (on) *(unsigned *)(n + 0x1c) |= 0x10000000u; else *(unsigned *)(n + 0x1c) &= ~0x10000000u;
}
static void f_node_set_collision(void *node, int on) {
    char *n = (char *)node; if (!readable(n + 0x32, 2)) return;
    if (on) *(unsigned short *)(n + 0x30) &= ~0x60; else *(unsigned short *)(n + 0x30) |= 0x60;
}
static int f_hero_pos(float *o) { char *h = (char *)get_hero(); if (!h) return 0; memcpy(o, h + OFF_WPOS, 12); return 1; }
static int f_hero_tp(const float *p) {
    char *h = (char *)get_hero(); if (!h) return 0;
    memcpy(h + OFF_LPOS, p, 12); memcpy(h + OFF_WPOS, p, 12);
    g_haveLast = 0;
    return 1;
}
static const char *f_level(void) { return g_cur_level; }

/* ===================== zdarzenia ===================== */
#define FEV_MAX 32
static KaoLevelFn g_flevel[FEV_MAX]; static int g_nflevel;
static KaoKeyFn g_fkey[FEV_MAX]; static int g_nfkey;
static void f_on_level(KaoLevelFn f) { if (f && g_nflevel < FEV_MAX) g_flevel[g_nflevel++] = f; }
static void f_on_key(KaoKeyFn f) { if (f && g_nfkey < FEV_MAX) g_fkey[g_nfkey++] = f; }
static int g_mod_block;
static void f_input_block(int on) { g_mod_block = on != 0; }

static void forge_level_watch(void) {                  /* new scene + Kao present for 1 s -> on_level */
    static void *last_scene, *last_hero; static DWORD since; static int fired = 1;
    void *s = f_scene(), *h = get_hero();
    if (s != last_scene || h != last_hero) { last_scene = s; last_hero = h; since = GetTickCount(); fired = 0; }
    if (!fired && s && h && GetTickCount() - since > 1000) {
        fired = 1;
        logf_("forge: level ready: %s\n", g_cur_level);
        for (int i = 0; i < g_nflevel; i++) g_flevel[i](g_cur_level);
    }
}

/* ===================== rysowanie ===================== */
static float g_pv[16]; static int g_pv_ok; static GLint g_pv_vp[4];
/* Engine (FUN_005746e0 / FUN_005b42c0, from the code): GL_PROJECTION = only P (gluPerspective); the camera view matrix V lives in the game
 * context's own matrix stack and after the scene is loaded (glLoadMatrixf @0x5b4333) into GL_MODELVIEW. So here: PROJECTION = P, MODELVIEW = V.
 * g_pv = P * V (column-major like GL) - for world->screen projection and for deriving the camera. */
static void forge_capture_pv(void) {
    GLfloat P[16], V[16]; GLint mode; static int logged;
    glGetFloatv(GL_PROJECTION_MATRIX, P); glGetFloatv(GL_MODELVIEW_MATRIX, V); glGetIntegerv(GL_MATRIX_MODE, &mode);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        float v = 0; for (int k = 0; k < 4; k++) v += P[k * 4 + r] * V[c * 4 + k];
        g_pv[c * 4 + r] = v;
    }
    glGetIntegerv(GL_VIEWPORT, g_pv_vp); g_pv_ok = 1;
    if (logged < 3) {
        logged++;
        logf_("forge: macierze tryb=%x\n  P : %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n  MV: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", mode,
              P[0],P[1],P[2],P[3],P[4],P[5],P[6],P[7],P[8],P[9],P[10],P[11],P[12],P[13],P[14],P[15],
              V[0],V[1],V[2],V[3],V[4],V[5],V[6],V[7],V[8],V[9],V[10],V[11],V[12],V[13],V[14],V[15]);
    }
}
/* Camera from P*V (column-major): the w-clip row = (m3,m7,m11,m15) gives the view direction; eye = solution of x=y=w=0. */
static int f_camera(float *eye, float *fwd) {
    if (!g_pv_ok) return 0;
    const float *m = g_pv;
    double r[3][4] = { { m[0], m[4], m[8], -m[12] }, { m[1], m[5], m[9], -m[13] }, { m[3], m[7], m[11], -m[15] } };
    double l = sqrt((double)m[3]*m[3] + (double)m[7]*m[7] + (double)m[11]*m[11]);
    if (l < 1e-9) return 0;
    fwd[0] = (float)(m[3] / l); fwd[1] = (float)(m[7] / l); fwd[2] = (float)(m[11] / l);
    for (int c = 0; c < 3; c++) {                        /* eliminacja Gaussa 3x3 z wyborem elementu */
        int piv = c; for (int i = c + 1; i < 3; i++) if (fabs(r[i][c]) > fabs(r[piv][c])) piv = i;
        if (fabs(r[piv][c]) < 1e-12) return 0;
        for (int k = 0; k < 4; k++) { double t = r[c][k]; r[c][k] = r[piv][k]; r[piv][k] = t; }
        for (int i = 0; i < 3; i++) if (i != c) { double f = r[i][c] / r[c][c]; for (int k = c; k < 4; k++) r[i][k] -= f * r[c][k]; }
    }
    for (int i = 0; i < 3; i++) eye[i] = (float)(r[i][3] / r[i][i]);
    return 1;
}
static int f_w2s(const float *w, float *sx, float *sy) {
    if (!g_pv_ok) return 0;
    const float *m = g_pv;                             /* kolumnowo (GL) */
    float x = m[0]*w[0] + m[4]*w[1] + m[8]*w[2] + m[12];
    float y = m[1]*w[0] + m[5]*w[1] + m[9]*w[2] + m[13];
    float W = m[3]*w[0] + m[7]*w[1] + m[11]*w[2] + m[15];
    if (W <= 1e-4f) return 0;
    *sx = (x / W * 0.5f + 0.5f) * g_pv_vp[2];
    *sy = (1.0f - (y / W * 0.5f + 0.5f)) * g_pv_vp[3];
    return 1;
}
static void f_rect(float x0, float y0, float x1, float y1) { rect(x0, y0, x1, y1); }

/* ===================== konfiguracja (kaoforge_mods.ini) ===================== */
static char g_ini[MAX_PATH], g_gamedir[MAX_PATH];
static const char *ini_path(void) {
    if (!g_ini[0]) { GetFullPathNameA("kaoforge_mods.ini", MAX_PATH, g_ini, NULL); GetFullPathNameA(".", MAX_PATH, g_gamedir, NULL); }
    return g_ini;
}
static int f_cfg_int(const char *s, const char *k, int d) { return GetPrivateProfileIntA(s, k, d, ini_path()); }
static const char *f_cfg_str(const char *s, const char *k, const char *d) {
    static char buf[4][512]; static int r; r = (r + 1) & 3;
    GetPrivateProfileStringA(s, k, d ? d : "", buf[r], sizeof buf[r], ini_path());
    return buf[r];
}
static float f_cfg_float(const char *s, const char *k, float d) {
    const char *v = f_cfg_str(s, k, ""); return *v ? (float)atof(v) : d;
}
static void f_cfg_set(const char *s, const char *k, const char *v) { WritePrivateProfileStringA(s, k, v, ini_path()); }
static const char *f_game_dir(void) { ini_path(); return g_gamedir; }

/* ===================== konsola ===================== */
#define CON_LINES 200
#define CMD_MAX 128
typedef struct { char name[32]; char help[96]; KaoCmdFn fn; int builtin; } FCmd;
static FCmd g_cmd[CMD_MAX]; static int g_ncmd;
static char g_con[CON_LINES][160]; static int g_ncon;
static char g_cin[160]; static int g_ncin;
static char g_hist[32][160]; static int g_nhist, g_hpos;
static int g_con_open;
static WNDPROC g_prev_wnd;

static void con_add(const char *s) {
    if (g_ncon == CON_LINES) { memmove(g_con[0], g_con[1], sizeof g_con[0] * (CON_LINES - 1)); g_ncon--; }
    snprintf(g_con[g_ncon++], sizeof g_con[0], "%s", s);
}
static void f_print(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    logf_("con: %s%s", b, (*b && b[strlen(b) - 1] == '\n') ? "" : "\n");
    char *p = b;
    for (char *q; (q = strchr(p, '\n')); p = q + 1) { *q = 0; con_add(p); }
    if (*p) con_add(p);
}
static void f_command(const char *name, const char *help, KaoCmdFn fn) {
    if (!name || !fn) return;
    for (int i = 0; i < g_ncmd; i++) if (!_stricmp(g_cmd[i].name, name)) { g_cmd[i].fn = fn; return; }
    if (g_ncmd >= CMD_MAX) return;
    FCmd *c = &g_cmd[g_ncmd++];
    snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->help, sizeof c->help, "%s", help ? help : "");
    c->fn = fn; c->builtin = g_forge_builtin_phase;
}
static int f_exec(const char *line) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", line);
    char *argv[16]; int argc = 0;
    for (char *t = strtok(buf, " \t"); t && argc < 16; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return 0;
    for (int i = 0; i < g_ncmd; i++) if (!_stricmp(g_cmd[i].name, argv[0])) { g_cmd[i].fn(argc, argv); return 1; }
    f_print("nieznana komenda: %s (help)", argv[0]);
    return 0;
}

static DWORD g_dist_dirty;              /* time of the last wheel zoom (saved after a moment of inactivity) */
static LRESULT CALLBACK forge_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEWHEEL && !g_menu && g_freecam) {                     /* mouse wheel: camera distance */
        int steps = (short)HIWORD(w) / 120;
        if (steps) {
            g_fc_dist *= powf(1.12f, (float)-steps); if (g_fc_dist < 120) g_fc_dist = 120; if (g_fc_dist > 3000) g_fc_dist = 3000;
            g_dist_saved = 1; g_dist_dirty = GetTickCount() | 1;
        }
        return 0;
    }
    if (m == WM_KEYDOWN || m == WM_KEYUP) {
        int down = m == WM_KEYDOWN;
        for (int i = 0; i < g_nfkey; i++) if (g_fkey[i]((int)w, down)) return 0;
    }
    return CallWindowProcA(g_prev_wnd, h, m, w, l);
}
static void forge_subclass(void) {
    if (g_prev_wnd) return;
    HWND hw = readable((void *)VA(ADDR_GAME_HWND), 4) ? *(HWND *)VA(ADDR_GAME_HWND) : NULL;
    if (!hw) return;
    g_prev_wnd = (WNDPROC)SetWindowLongA(hw, GWL_WNDPROC, (LONG)forge_wndproc);
}


/* ---- console autocomplete: commands (first token) and level names after "load " ---- */
#define CON_MAXC 24
static char g_tab_base[160], g_tab_last[160]; static int g_tab_i = -1;
static int con_cands(const char *line, char full[][200], char disp[][40], const char **help) {
    int n = 0; const char *sp = strchr(line, ' ');
    *help = NULL;
    if (!sp) {
        size_t L = strlen(line);
        for (int i = 0; i < g_ncmd && n < CON_MAXC; i++)
            if (!_strnicmp(g_cmd[i].name, line, L)) { snprintf(full[n], 200, "%s", g_cmd[i].name); snprintf(disp[n++], 40, "%s", g_cmd[i].name); }
        for (int i = 0; i < g_ncmd; i++) if (!_stricmp(g_cmd[i].name, line)) *help = g_cmd[i].help;
        if (!*help && n) for (int i = 0; i < g_ncmd; i++) if (!_stricmp(g_cmd[i].name, full[0])) *help = g_cmd[i].help;
    } else {
        char cmd[40]; snprintf(cmd, sizeof cmd, "%.*s", (int)(sp - line), line);
        for (int i = 0; i < g_ncmd; i++) if (!_stricmp(g_cmd[i].name, cmd)) *help = g_cmd[i].help;
        const char *arg = line + strlen(line); while (arg > line && arg[-1] != ' ') arg--;
        if (!_stricmp(cmd, "load") && arg == sp + 1) {
            int total = *(int *)VA(ADDR_LVL_COUNT); size_t L = strlen(arg);
            for (int i = 0; i < total && i < 64 && n < CON_MAXC; i++) {
                const char *m = level_name(i);
                if (m && !_strnicmp(m, arg, L)) { snprintf(full[n], 200, "load %s", m); snprintf(disp[n++], 40, "%s", m); }
            }
        }
    }
    return n;
}
static void con_tab(void) {
    if (strcmp(g_cin, g_tab_last) != 0 || g_tab_i < 0) { snprintf(g_tab_base, sizeof g_tab_base, "%s", g_cin); g_tab_i = -1; }
    char full[CON_MAXC][200], disp[CON_MAXC][40]; const char *help;
    int n = con_cands(g_tab_base, full, disp, &help);
    if (!n) return;
    g_tab_i = (g_tab_i + 1) % n;
    snprintf(g_cin, sizeof g_cin, "%s%s", full[g_tab_i], n == 1 && !strchr(full[0], ' ') ? " " : "");
    g_ncin = (int)strlen(g_cin);
    snprintf(g_tab_last, sizeof g_tab_last, "%s", g_cin);
}
/* The console reads keys with GetAsyncKeyState - the game swallows window key messages (DirectInput + its own wndproc). */
static void con_submit(void) {
    g_cin[g_ncin] = 0;
    if (g_ncin) {
        f_print("> %s", g_cin);
        if (g_nhist == 32) { memmove(g_hist[0], g_hist[1], sizeof g_hist[0] * 31); g_nhist--; }
        snprintf(g_hist[g_nhist++], sizeof g_hist[0], "%s", g_cin);
        f_exec(g_cin);
    }
    g_ncin = 0; g_cin[0] = 0; g_hpos = g_nhist;
}
static void forge_console_poll(void) {
    if (pressed(VK_OEM_3) || pressed(VK_F10)) { g_con_open ^= 1; logf_("forge: console %s\n", g_con_open ? "opened" : "closed"); return; }
    if (!g_con_open) return;
    HWND hw = readable((void *)VA(ADDR_GAME_HWND), 4) ? *(HWND *)VA(ADDR_GAME_HWND) : NULL;
    if (hw && GetForegroundWindow() != hw) return;
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (int vk = 'A'; vk <= 'Z'; vk++) if (pressed(vk) && g_ncin < (int)sizeof g_cin - 1) { g_cin[g_ncin++] = (char)(shift ? vk : vk + 32); g_cin[g_ncin] = 0; }
    for (int vk = '0'; vk <= '9'; vk++) if (pressed(vk) && g_ncin < (int)sizeof g_cin - 1) { g_cin[g_ncin++] = (char)vk; g_cin[g_ncin] = 0; }
    static const struct { int vk; char c, cs; } sym[] = { { VK_SPACE, ' ', ' ' }, { VK_OEM_PERIOD, '.', '.' }, { VK_OEM_MINUS, '-', '_' },
        { VK_OEM_COMMA, ',', ',' }, { VK_OEM_2, '/', '?' }, { VK_OEM_PLUS, '=', '+' }, { VK_OEM_5, '\\', '|' } };
    for (int i = 0; i < 7; i++) if (pressed(sym[i].vk) && g_ncin < (int)sizeof g_cin - 1) { g_cin[g_ncin++] = shift ? sym[i].cs : sym[i].c; g_cin[g_ncin] = 0; }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; vk++) if (pressed(vk) && g_ncin < (int)sizeof g_cin - 1) { g_cin[g_ncin++] = (char)('0' + vk - VK_NUMPAD0); g_cin[g_ncin] = 0; }
    static DWORD bs_t; DWORD now = GetTickCount();
    if (pressed(VK_BACK)) { if (g_ncin) g_cin[--g_ncin] = 0; bs_t = now + 350; }
    else if ((GetAsyncKeyState(VK_BACK) & 0x8000) && now > bs_t && g_ncin) { g_cin[--g_ncin] = 0; bs_t = now + 40; }
    if (pressed(VK_TAB)) con_tab();
    if (pressed(VK_RETURN)) con_submit();
    if (pressed(VK_ESCAPE)) g_con_open = 0;
    if (pressed(VK_UP) && g_nhist) { if (g_hpos > 0) g_hpos--; snprintf(g_cin, sizeof g_cin, "%s", g_hist[g_hpos]); g_ncin = (int)strlen(g_cin); }
    if (pressed(VK_DOWN) && g_nhist) { if (g_hpos < g_nhist - 1) { g_hpos++; snprintf(g_cin, sizeof g_cin, "%s", g_hist[g_hpos]); } else { g_hpos = g_nhist; g_cin[0] = 0; } g_ncin = (int)strlen(g_cin); }
}
static int forge_input_blocked(void) { return g_con_open || g_mod_block; }

static void forge_console_draw(int W, int H) {
    if (!g_con_open || !font_ready()) return;
    int rows = 18; float h = 26.0f * rows + 40;
    glColor4f(0.03f, 0.04f, 0.06f, 0.88f); rect(0, 0, (float)W, h);
    glColor4f(1.0f, 0.62f, 0.15f, 1); rect(0, h - 3, (float)W, h);
    int first = g_ncon - rows; if (first < 0) first = 0;
    for (int i = first, y = 0; i < g_ncon; i++, y++) {
        glColor4f(g_con[i][0] == '>' ? 1.0f : 0.85f, g_con[i][0] == '>' ? 0.8f : 0.88f, g_con[i][0] == '>' ? 0.4f : 0.9f, 1);
        text_at(12, 26.0f + y * 26.0f, g_con[i]);
    }
    char b[200]; snprintf(b, sizeof b, "] %s%s", g_cin, (GetTickCount() / 400) & 1 ? "_" : "");
    glColor4f(1, 1, 1, 1); text_at(12, h - 12, b);
    {   /* suggestions under the console */
        const char *base = (g_tab_i >= 0 && !strcmp(g_cin, g_tab_last)) ? g_tab_base : g_cin;
        char full[CON_MAXC][200], disp[CON_MAXC][40]; const char *help;
        int n = con_cands(base, full, disp, &help);
        if (g_ncin && (n || help)) {
            float y0 = h + 4, rowh = 28;
            glColor4f(0.03f, 0.04f, 0.06f, 0.92f); rect(0, y0, (float)W, y0 + rowh * (help ? 2 : 1) + 6);
            float x = 14; int sel = (g_tab_i >= 0 && !strcmp(g_cin, g_tab_last)) ? g_tab_i : (n == 1 ? 0 : -1);
            for (int k = 0; k < n; k++) {
                float tw = 11.0f * (float)strlen(disp[k]) + 22;
                if (x + tw > W - 10) { glColor4f(0.6f, 0.6f, 0.65f, 1); text_at(x, y0 + 22, "..."); break; }
                if (k == sel) { glColor4f(1.0f, 0.62f, 0.15f, 0.35f); rect(x - 6, y0 + 3, x + tw - 8, y0 + rowh); }
                glColor4f(k == sel ? 1.0f : 0.75f, k == sel ? 0.85f : 0.8f, k == sel ? 0.4f : 0.85f, 1);
                text_at(x, y0 + 22, disp[k]);
                x += tw;
            }
            if (help) { glColor4f(0.55f, 0.9f, 1.0f, 1); text_at(14, y0 + rowh + 22, help); }
            glColor4f(0.5f, 0.5f, 0.55f, 1); text_at((float)W - 190, y0 + 22, "Tab = uzupelnij");
        }
    }
    (void)H;
}

/* ===================== komendy wbudowane ===================== */
static void c_help(int c, char **v) {
    (void)c; (void)v;
    for (int i = 0; i < g_ncmd; i++) f_print("%-10s %s", g_cmd[i].name, g_cmd[i].help);
}
static void c_pos(int c, char **v) { (void)c; (void)v; float p[3]; if (f_hero_pos(p)) f_print("Kao: %.1f %.1f %.1f  (%s)", p[0], p[1], p[2], g_cur_level); else f_print("no Kao"); }
static void c_tp(int c, char **v) {
    float p[3];
    if (c == 2) { void *n = f_find(v[1]); if (!n || !f_node_pos(n, p)) { f_print("no node %s", v[1]); return; } p[2] += 50; }
    else if (c == 4) { p[0] = (float)atof(v[1]); p[1] = (float)atof(v[2]); p[2] = (float)atof(v[3]); }
    else { f_print("tp x y z  |  tp <node name>"); return; }
    f_print(f_hero_tp(p) ? "teleport %.0f %.0f %.0f" : "no Kao", p[0], p[1], p[2]);
}
static void c_load(int c, char **v) { if (c < 2) { f_print("load <level> (see levels)"); return; } load_level(v[1]); }
static void c_levels(int c, char **v) {
    (void)c; (void)v; int n = *(int *)VA(ADDR_LVL_COUNT);
    for (int i = 0; i < n && i < 64; i++) { const char *m = level_name(i); if (m) f_print("%2d %s", i, m); }
}
static void c_toggle(int c, char **v) {
    int *t = !_stricmp(v[0], "god") ? &g_god : !_stricmp(v[0], "fly") ? &g_fly : !_stricmp(v[0], "fast") ? &g_fast :
             !_stricmp(v[0], "jump") ? &g_jump : !_stricmp(v[0], "freecam") ? &g_freecam : NULL;
    if (!t) return;
    *t = c > 1 ? atoi(v[1]) != 0 : !*t;
    f_print("%s = %d", v[0], *t);
}
typedef struct { const char *pat; int n, shown, depth; } FindCtx;
static int find_cb(void *node, int depth, void *ctx) {
    FindCtx *f = (FindCtx *)ctx;
    if (f->depth >= 0 && depth > f->depth) return 2;
    const char *nm = node_name((char *)node);
    if (!f->pat || strstr(nm, f->pat)) {
        f->n++;
        if (f->shown < 60) {
            float p[3] = {0, 0, 0}; f_node_pos(node, p);
            f_print("%*s%s \"%s\" @%p (%.0f %.0f %.0f)", f->depth >= 0 ? depth * 2 : 0, "", rtti_name(node) + 4, nm, node, p[0], p[1], p[2]);
            f->shown++;
        }
    }
    return 0;
}
static void c_find(int c, char **v) { FindCtx f = { c > 1 ? v[1] : NULL, 0, 0, -1 }; f_walk(NULL, find_cb, &f); f_print("znaleziono %d", f.n); }
static void c_tree(int c, char **v) { FindCtx f = { NULL, 0, 0, c > 1 ? atoi(v[1]) : 2 }; f_walk(NULL, find_cb, &f); f_print("nodes up to depth %d: %d", f.depth, f.n); }
static void c_vis(int c, char **v) {
    if (c < 2) { f_print("%s <node>", v[0]); return; }
    void *n = f_find(v[1]); if (!n) { f_print("no node %s", v[1]); return; }
    if (!_stricmp(v[0], "hide")) f_node_set_visible(n, 0); else if (!_stricmp(v[0], "show")) f_node_set_visible(n, 1);
    else if (!_stricmp(v[0], "nocol")) f_node_set_collision(n, 0); else f_node_set_collision(n, 1);
    f_print("%s %s", v[0], v[1]);
}
static void c_peek(int c, char **v) {
    if (c < 2) { f_print("peek <adres hex> [bajty]   (adresy gry: dodaj g, np. 0xB3D868g)"); return; }
    unsigned a = strtoul(v[1], NULL, 16); if (strchr(v[1], 'g')) a = (unsigned)VA(a);
    int n = c > 2 ? atoi(v[2]) : 32; if (n > 256) n = 256;
    for (int o = 0; o < n; o += 16) {
        char b[100]; int k = snprintf(b, sizeof b, "%08x:", a + o);
        for (int i = 0; i < 16 && o + i < n; i++) k += snprintf(b + k, sizeof b - k, readable((void *)(a + o + i), 1) ? " %02x" : " ??",
                                                        readable((void *)(a + o + i), 1) ? *(BYTE *)(a + o + i) : 0);
        f_print("%s", b);
    }
}
static void c_poke(int c, char **v) {
    if (c < 3) { f_print("poke <adres hex>[g] <bajty hex...>"); return; }
    unsigned a = strtoul(v[1], NULL, 16); if (strchr(v[1], 'g')) a = (unsigned)VA(a);
    BYTE b[16]; int n = 0;
    for (int i = 2; i < c && n < 16; i++) b[n++] = (BYTE)strtoul(v[i], NULL, 16);
    f_print(f_write((void *)a, b, n) == 0 ? "wrote %d B" : "write failed", n);
}
static void c_speed(int c, char **v) {
    if (c > 1) { float x = (float)atof(v[1]); if (x < SPEED_MIN) x = SPEED_MIN; if (x > SPEED_MAX) x = SPEED_MAX; g_speed = x; settings_save(); }
    f_print("movement speed = %.2fx (range %.2f-%.0f)", g_speed, SPEED_MIN, SPEED_MAX);
}
static void c_flyspeed(int c, char **v) {
    if (c > 1) { float x = (float)atof(v[1]); g_flyspd = x < SPEED_MIN ? SPEED_MIN : x > SPEED_MAX ? SPEED_MAX : x; settings_save(); }
    f_print("flight speed = %.2fx", g_flyspd);
}
static void c_jumph(int c, char **v) {
    if (c > 1) { float x = (float)atof(v[1]); g_jumph = x < 0.25f ? 0.25f : x > 6.0f ? 6.0f : x; settings_save(); }
    f_print("jump height = %.2fx", g_jumph);
}
static void c_camdist(int c, char **v) {
    if (c > 1) { float x = (float)atof(v[1]); g_fc_dist = x < 120 ? 120 : x > 3000 ? 3000 : x; g_dist_saved = 1; settings_save(); }
    f_print("camera distance = %.0f (the mouse wheel changes it too)", g_fc_dist);
}
static void c_echo(int c, char **v) { char b[256] = ""; for (int i = 1; i < c; i++) { strcat(b, v[i]); strcat(b, " "); } f_print("%s", b); }
static void forge_reload(void);
static void c_mods(int c, char **v);
static void c_reload(int c, char **v) { (void)c; (void)v; forge_reload(); }
static void c_exec(int c, char **v) {
    if (c < 2) { f_print("exec <file>  (command lines, e.g. autoexec.cfg)"); return; }
    FILE *f = fopen(v[1], "r"); if (!f) { f_print("no file %s", v[1]); return; }
    char l[256]; while (fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (*l && *l != '#' && *l != '/') f_exec(l); }
    fclose(f);
}

static void forge_builtin_commands(void) {
    g_forge_builtin_phase = 1;
    f_command("help", "lista komend", c_help);
    f_command("pos", "Kao's position", c_pos);
    f_command("tp", "teleport: tp x y z | tp <node>", c_tp);
    f_command("load", "load a level: load <name>", c_load);
    f_command("levels", "list levels", c_levels);
    f_command("god", "god mode [0/1]", c_toggle);
    f_command("fly", "fly mode [0/1]", c_toggle);
    f_command("fast", "fast movement [0/1]", c_toggle);
    f_command("jump", "high jump [0/1]", c_toggle);
    f_command("freecam", "free camera [0/1]", c_toggle);
    f_command("find", "find nodes: find [part of name]", c_find);
    f_command("tree", "scene tree: tree [depth]", c_tree);
    f_command("hide", "hide a node", c_vis); f_command("show", "show a node", c_vis);
    f_command("nocol", "disable a node's collision", c_vis); f_command("col", "enable a node's collision", c_vis);
    f_command("peek", "memory: peek <addr>[g] [n]", c_peek);
    f_command("poke", "write: poke <addr>[g] <bytes>", c_poke);
    f_command("speed", "movement speed: speed [multiplier 0.25-8]", c_speed);
    f_command("flyspeed", "flight speed: flyspeed [0.25-8]", c_flyspeed);
    f_command("jumph", "jump height: jumph [0.25-6]", c_jumph);
    f_command("camdist", "camera distance: camdist [120-3000]", c_camdist);
    f_command("echo", "wypisz tekst", c_echo);
    f_command("mods", "loaded mods", c_mods);
    f_command("reload", "reload mods from mods/ (hot-reload)", c_reload);
    f_command("exec", "run a command file", c_exec);
    g_forge_builtin_phase = 0;
}
