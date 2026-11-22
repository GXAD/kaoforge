/*
 * gfx.c - graphics improvements for Kao2 (#included from kaoforge.c).
 *
 * Engine: OpenGL 1.x fixed function (display lists, vertex arrays, gluBuild2DMipmaps, no shaders).
 * 1) Texture filtering: IAT hook of OPENGL32!glTexParameteri -> trilinear + 16x anisotropy.
 * 2) Soft shadows (F11): the game's shadow is a black quad (alpha 0.5) drawn through the stencil mask (FUN_0047f490:
 *    glStencilFunc(GL_NOTEQUAL,0) ... quad ... glStencilFunc(GL_ALWAYS)). IAT hook of glStencilFunc:
 *    on NOTEQUAL we copy the frame "without shadow" (A), on ALWAYS the frame "with shadow" (B) and draw
 *    A * (1 - 0.5 * blurred_mask), mask = (A != B). This gives a penumbra instead of a hard edge.
 * 3) Post-processing (F8): light FXAA + a little sharpening.
 */
#include <GL/gl.h>
#include <math.h>

#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#define GL_CLAMP_TO_EDGE   0x812F
#define GL_TEXTURE1        0x84C1
#define GL_TEXTURE0        0x84C0
typedef char GLchar;

typedef void   (APIENTRY *PFN_TexParameteri)(GLenum, GLenum, GLint);
typedef void   (APIENTRY *PFN_StencilFunc)(GLenum, GLint, GLuint);
typedef GLuint (APIENTRY *PFN_CreateShader)(GLenum);
typedef void   (APIENTRY *PFN_ShaderSource)(GLuint, GLsizei, const GLchar **, const GLint *);
typedef void   (APIENTRY *PFN_CompileShader)(GLuint);
typedef GLuint (APIENTRY *PFN_CreateProgram)(void);
typedef void   (APIENTRY *PFN_AttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_LinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_UseProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_GetUniformLocation)(GLuint, const GLchar *);
typedef void   (APIENTRY *PFN_Uniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_Uniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_Uniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (APIENTRY *PFN_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_ActiveTexture)(GLenum);

static PFN_TexParameteri real_glTexParameteri;
static PFN_StencilFunc real_glStencilFunc;
static float g_maxAniso = -1.0f;
static int g_gfx_on = 1, g_soft_on = 1;
static int g_gl_state;                /* 0 = not tried, 1 = ready, -1 = no GL2 */
static int g_texhooks, g_shadow_pending, g_shadow_frames;

static PFN_CreateShader p_CreateShader; static PFN_ShaderSource p_ShaderSource; static PFN_CompileShader p_CompileShader;
static PFN_CreateProgram p_CreateProgram; static PFN_AttachShader p_AttachShader; static PFN_LinkProgram p_LinkProgram;
static PFN_UseProgram p_UseProgram; static PFN_GetUniformLocation p_GetUniformLocation; static PFN_Uniform1i p_Uniform1i;
static PFN_Uniform2f p_Uniform2f; static PFN_Uniform1f p_Uniform1f; static PFN_GetShaderiv p_GetShaderiv;
static PFN_GetShaderInfoLog p_GetShaderInfoLog; static PFN_GetProgramiv p_GetProgramiv; static PFN_ActiveTexture p_ActiveTexture;

static GLuint g_prog_pp, g_prog_sh;
static GLuint g_texA, g_texB;
static int g_tw, g_th;

/* ---------- 1) texture filtering ---------- */
static void APIENTRY hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (pname == GL_TEXTURE_MIN_FILTER &&
        (param == GL_LINEAR_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR))
        param = GL_LINEAR_MIPMAP_LINEAR;
    real_glTexParameteri(target, pname, param);
    if (pname == GL_TEXTURE_MIN_FILTER && target == GL_TEXTURE_2D) {
        if (g_maxAniso < 0) {
            const char *ext = (const char *)glGetString(GL_EXTENSIONS);
            g_maxAniso = 0;
            if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic")) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_maxAniso);
            if (g_maxAniso > 16) g_maxAniso = 16;
            logf_("gfx: anizotropia max = %.0f\n", g_maxAniso);
        }
        if (g_maxAniso > 1 && param != GL_LINEAR && param != GL_NEAREST)
            glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_maxAniso);
        g_texhooks++;
    }
}

/* replace an entry in a module's import table (looked up by DLL and function name) */
static void *iat_hook(HMODULE mod, const char *dll, const char *func, void *repl) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    void *target = (void *)GetProcAddress(GetModuleHandleA(dll), func);
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        IMAGE_THUNK_DATA *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk) : NULL;
        for (int i = 0; iat[i].u1.Function; i++) {
            int match = 0;
            if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
                match = strcmp((char *)ibn->Name, func) == 0;
            } else if (target) {
                match = (void *)iat[i].u1.Function == target;
            }
            if (match) {
                DWORD old;
                void *orig = (void *)iat[i].u1.Function;
                VirtualProtect(&iat[i].u1.Function, 4, PAGE_READWRITE, &old);
                iat[i].u1.Function = (DWORD_PTR)repl;
                VirtualProtect(&iat[i].u1.Function, 4, old, &old);
                return orig;
            }
        }
    }
    return NULL;
}

/* ---------- shadery ---------- */
static const char *FRAG_PP =
"#version 110\n"
"uniform sampler2D tex; uniform vec2 px;\n"
"vec3 S(vec2 o){ return texture2D(tex, o).rgb; }\n"
"float L(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
"void main(){\n"
"  vec2 uv = gl_TexCoord[0].xy;\n"
"  vec3 cM = S(uv);\n"
"  vec3 cNW=S(uv+vec2(-1.,-1.)*px), cNE=S(uv+vec2(1.,-1.)*px), cSW=S(uv+vec2(-1.,1.)*px), cSE=S(uv+vec2(1.,1.)*px);\n"
"  float lNW=L(cNW), lNE=L(cNE), lSW=L(cSW), lSE=L(cSE), lM=L(cM);\n"
"  float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))), lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));\n"
"  vec2 dir = vec2(-((lNW+lNE)-(lSW+lSE)), ((lNW+lSW)-(lNE+lSE)));\n"
"  float red = max((lNW+lNE+lSW+lSE)*0.03125, 1.0/128.0);\n"
"  float rcp = 1.0/(min(abs(dir.x),abs(dir.y))+red);\n"
"  dir = clamp(dir*rcp, vec2(-8.0), vec2(8.0))*px;\n"
"  vec3 rA = 0.5*(S(uv+dir*(1.0/3.0-0.5))+S(uv+dir*(2.0/3.0-0.5)));\n"
"  vec3 rB = rA*0.5+0.25*(S(uv+dir*-0.5)+S(uv+dir*0.5));\n"
"  float lB = L(rB);\n"
"  vec3 c = (lB<lMin || lB>lMax) ? rA : rB;\n"
"  vec3 blur = (cNW+cNE+cSW+cSE)*0.25;\n"
"  c = clamp(c + (c-blur)*0.15, 0.0, 1.0);\n"
"  gl_FragColor = vec4(c,1.0);\n"
"}\n";

/* A = frame without the shadow, B = with the game's hard shadow. Mask = A!=B, blurred with a Poisson disc. */
static const char *FRAG_SH =
"#version 110\n"
"uniform sampler2D texA; uniform sampler2D texB; uniform vec2 px; uniform float rad; uniform float alpha;\n"
"float M(vec2 o){ vec3 d = texture2D(texA,o).rgb - texture2D(texB,o).rgb; return step(0.006, max(d.r,max(d.g,d.b))); }\n"
"void main(){\n"
"  vec2 uv = gl_TexCoord[0].xy;\n"
"  vec2 k[16];\n"
"  k[0]=vec2(-0.94,-0.40); k[1]=vec2(0.95,-0.77); k[2]=vec2(-0.09,-0.93); k[3]=vec2(0.34,0.29);\n"
"  k[4]=vec2(-0.92,0.46); k[5]=vec2(-0.82,-0.88); k[6]=vec2(-0.38,0.28); k[7]=vec2(0.97,0.76);\n"
"  k[8]=vec2(0.44,-0.97); k[9]=vec2(0.54,-0.47); k[10]=vec2(-0.26,-0.42); k[11]=vec2(-0.24,0.99);\n"
"  k[12]=vec2(-0.81,0.15); k[13]=vec2(0.20,0.79); k[14]=vec2(0.79,0.19); k[15]=vec2(0.0,0.0);\n"
"  float m = 0.0;\n"
"  for (int i=0;i<16;i++){ m += M(uv + k[i]*px*rad) + M(uv + k[i]*px*rad*0.45); }\n"
"  m *= 1.0/32.0;\n"
"  m = smoothstep(0.0, 1.0, m);\n"
"  vec3 a = texture2D(texA, uv).rgb;\n"
"  gl_FragColor = vec4(a * (1.0 - alpha*m), 1.0);\n"
"}\n";

#define GETP(var, name) var = (void *)wglGetProcAddress(name); if (!var) { logf_("gfx: missing %s\n", name); return 0; }

static GLuint make_prog(const char *src, const char *nm) {
    GLuint fs = p_CreateShader(GL_FRAGMENT_SHADER);
    p_ShaderSource(fs, 1, &src, NULL);
    p_CompileShader(fs);
    GLint ok = 0; p_GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char buf[2048]; p_GetShaderInfoLog(fs, sizeof buf, NULL, buf); logf_("gfx: shader %s error:\n%s\n", nm, buf); return 0; }
    GLuint p = p_CreateProgram();
    p_AttachShader(p, fs);
    p_LinkProgram(p);
    p_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { logf_("gfx: link %s error\n", nm); return 0; }
    return p;
}

static int gl_init(void) {
    GETP(p_CreateShader, "glCreateShader"); GETP(p_ShaderSource, "glShaderSource"); GETP(p_CompileShader, "glCompileShader");
    GETP(p_CreateProgram, "glCreateProgram"); GETP(p_AttachShader, "glAttachShader"); GETP(p_LinkProgram, "glLinkProgram");
    GETP(p_UseProgram, "glUseProgram"); GETP(p_GetUniformLocation, "glGetUniformLocation"); GETP(p_Uniform1i, "glUniform1i");
    GETP(p_Uniform2f, "glUniform2f"); GETP(p_Uniform1f, "glUniform1f"); GETP(p_GetShaderiv, "glGetShaderiv");
    GETP(p_GetShaderInfoLog, "glGetShaderInfoLog"); GETP(p_GetProgramiv, "glGetProgramiv"); GETP(p_ActiveTexture, "glActiveTexture");
    g_prog_pp = make_prog(FRAG_PP, "pp");
    g_prog_sh = make_prog(FRAG_SH, "shadow");
    if (!g_prog_pp || !g_prog_sh) return 0;
    glGenTextures(1, &g_texA);
    glGenTextures(1, &g_texB);
    logf_("gfx: shadery gotowe (GL %s, %s)\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return 1;
}

static int gl_ready(void) {
    if (g_gl_state == 0) g_gl_state = gl_init() ? 1 : -1;
    return g_gl_state == 1;
}

static void tex_setup(GLuint t, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    (real_glTexParameteri ? real_glTexParameteri : glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* copy the current back buffer into a texture; returns 0 when the size is unknown */
static int grab(GLuint t, GLint *vp) {
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return 0;
    if (vp[2] != g_tw || vp[3] != g_th) {
        tex_setup(g_texA, vp[2], vp[3]); tex_setup(g_texB, vp[2], vp[3]);
        g_tw = vp[2]; g_th = vp[3];
        logf_("gfx: bufory %dx%d\n", g_tw, g_th);
    }
    glBindTexture(GL_TEXTURE_2D, t);
    glReadBuffer(GL_BACK);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp[0], vp[1], vp[2], vp[3]);
    return 1;
}

static void fullscreen_quad(void) {
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glEnable(GL_TEXTURE_2D);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* ---------- 2) soft shadows ---------- */
static void APIENTRY hook_glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    if (g_soft_on && func == GL_NOTEQUAL && ref == 0 && gl_ready()) {
        GLint vp[4], bound;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        g_shadow_pending = grab(g_texA, vp);             /* frame before the shadow */
        glBindTexture(GL_TEXTURE_2D, bound);
    } else if (func == GL_ALWAYS && g_shadow_pending) {
        g_shadow_pending = 0;
        GLint vp[4], bound;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        if (grab(g_texB, vp)) {                          /* frame with the game's hard shadow */
            p_ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_texB);
            p_ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_texA);
            p_UseProgram(g_prog_sh);
            p_Uniform1i(p_GetUniformLocation(g_prog_sh, "texA"), 0);
            p_Uniform1i(p_GetUniformLocation(g_prog_sh, "texB"), 1);
            p_Uniform2f(p_GetUniformLocation(g_prog_sh, "px"), 1.0f / vp[2], 1.0f / vp[3]);
            p_Uniform1f(p_GetUniformLocation(g_prog_sh, "rad"), 9.0f * vp[3] / 1080.0f);
            p_Uniform1f(p_GetUniformLocation(g_prog_sh, "alpha"), 0.5f);   /* alfa cienia gry (0x96ae34) */
            fullscreen_quad();
            p_UseProgram(0);
            p_ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
            p_ActiveTexture(GL_TEXTURE0);
            g_shadow_frames++;
        }
        glPopAttrib();
        glBindTexture(GL_TEXTURE_2D, bound);
    }
    real_glStencilFunc(func, ref, mask);
}

static void gfx_install_hooks(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    void *o = iat_hook(exe, "OPENGL32.dll", "glTexParameteri", (void *)hook_glTexParameteri);
    if (o) real_glTexParameteri = (PFN_TexParameteri)o;
    void *s = iat_hook(exe, "OPENGL32.dll", "glStencilFunc", (void *)hook_glStencilFunc);
    if (s) real_glStencilFunc = (PFN_StencilFunc)s;
    logf_("gfx: hook glTexParameteri %s, glStencilFunc %s\n", o ? "OK" : "MISSING", s ? "OK" : "MISSING");
}

/* ---------- 3) post-processing ---------- */
static void gfx_frame(void) {
    if (!g_gfx_on || !gl_ready()) return;
    GLint vp[4], bound;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (grab(g_texA, vp)) {
        p_UseProgram(g_prog_pp);
        p_Uniform1i(p_GetUniformLocation(g_prog_pp, "tex"), 0);
        p_Uniform2f(p_GetUniformLocation(g_prog_pp, "px"), 1.0f / vp[2], 1.0f / vp[3]);
        fullscreen_quad();
        p_UseProgram(0);
    }
    glPopAttrib();
    glBindTexture(GL_TEXTURE_2D, bound);
}
