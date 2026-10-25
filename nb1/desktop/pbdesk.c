/* pbdesk: desktop viewer for the NB-1 depth scene (SDL2 + OpenGL 3.3 core).
 *
 *   pbdesk --roms DIR (--ipc DIR | --state FILE) [--table T.json] [--res WxH] [--yaw N] [--pitch N] [--dist N] [--focus N] [--focal N]
 *          [--hide-hud] [--shot out.ppm [--shot-after N]]
 *
 * --ipc   directory written by tools/play/pbplay.lua (MAME): state files in, aim.txt out (the same protocol as pbview.py)
 * --state a single saved state (229,464 bytes as tools/mame/snap.lua dumps it, or the 8 + 229,464 + 4 byte published form)
 *
 * Keys: 1 straight arcade view   2 orbit camera   A/D yaw   W/S pitch   Q/E distance   Z/X focus depth   right-drag (or Alt+drag) orbit
 *       wheel distance   H hide HUD   T depth table on/off   R reload table   P pause   F5 screenshot (PPM)   F1 help   ESC quit
 *       C coin   V start   left mouse = trigger (the mouse ray is cast into the scene and snapped to a pixel the game can see)
 *
 * The scene comes from the nb1 library, so what you see is what the library computes (checked against the Python oracle). Known
 * approximation: sprite "shadow" pens (fades) are drawn as a plain 50% darkening here, not through the palette.
 */
#include "../nb1.h"
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#include <SDL.h>
#include <SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "font7x13.h"

#ifdef _WIN32
#include <direct.h>
#endif

/* ---------------------------------------------------------------- GL function loading (shaders, buffers, VAOs) */
#ifndef APIENTRY
#define APIENTRY
#endif
/* Our own pointer types and names: the PFN typedefs and prototypes differ between the Linux, Windows and Apple headers. */
typedef GLuint (APIENTRY *fn_CreateShader)(GLenum); typedef void (APIENTRY *fn_ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void (APIENTRY *fn_CompileShader)(GLuint); typedef void (APIENTRY *fn_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *fn_GetInfoLog)(GLuint, GLsizei, GLsizei *, char *); typedef GLuint (APIENTRY *fn_CreateProgram)(void);
typedef void (APIENTRY *fn_AttachShader)(GLuint, GLuint); typedef void (APIENTRY *fn_LinkProgram)(GLuint);
typedef void (APIENTRY *fn_GetProgramiv)(GLuint, GLenum, GLint *); typedef void (APIENTRY *fn_UseProgram)(GLuint);
typedef GLint (APIENTRY *fn_GetUniformLocation)(GLuint, const char *); typedef void (APIENTRY *fn_Uniform1i)(GLint, GLint);
typedef void (APIENTRY *fn_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat); typedef void (APIENTRY *fn_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (APIENTRY *fn_GenArrays)(GLsizei, GLuint *); typedef void (APIENTRY *fn_BindVertexArray)(GLuint);
typedef void (APIENTRY *fn_BindBuffer)(GLenum, GLuint); typedef void (APIENTRY *fn_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *fn_EnableVertexAttribArray)(GLuint); typedef void (APIENTRY *fn_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *fn_DeleteShader)(GLuint);
#define GL_FUNCS(X) \
    X(fn_CreateShader, glCreateShader) X(fn_ShaderSource, glShaderSource) X(fn_CompileShader, glCompileShader) X(fn_GetShaderiv, glGetShaderiv) \
    X(fn_GetInfoLog, glGetShaderInfoLog) X(fn_CreateProgram, glCreateProgram) X(fn_AttachShader, glAttachShader) X(fn_LinkProgram, glLinkProgram) \
    X(fn_GetProgramiv, glGetProgramiv) X(fn_GetInfoLog, glGetProgramInfoLog) X(fn_UseProgram, glUseProgram) X(fn_GetUniformLocation, glGetUniformLocation) \
    X(fn_Uniform1i, glUniform1i) X(fn_Uniform4f, glUniform4f) X(fn_UniformMatrix4fv, glUniformMatrix4fv) X(fn_GenArrays, glGenVertexArrays) \
    X(fn_BindVertexArray, glBindVertexArray) X(fn_GenArrays, glGenBuffers) X(fn_BindBuffer, glBindBuffer) X(fn_BufferData, glBufferData) \
    X(fn_EnableVertexAttribArray, glEnableVertexAttribArray) X(fn_VertexAttribPointer, glVertexAttribPointer) X(fn_DeleteShader, glDeleteShader)
#define X(T, N) static T pfn_##N;
GL_FUNCS(X)
#undef X
static int load_gl(void) {
#define X(T, N) pfn_##N = (T)SDL_GL_GetProcAddress(#N); if (!pfn_##N) { fprintf(stderr, "pbdesk: missing GL function %s\n", #N); return -1; }
    GL_FUNCS(X)
#undef X
    return 0;
}
/* from here on the plain names call our pointers (any header prototypes of the same names are no longer used) */
#define glCreateShader pfn_glCreateShader
#define glShaderSource pfn_glShaderSource
#define glCompileShader pfn_glCompileShader
#define glGetShaderiv pfn_glGetShaderiv
#define glGetShaderInfoLog pfn_glGetShaderInfoLog
#define glCreateProgram pfn_glCreateProgram
#define glAttachShader pfn_glAttachShader
#define glLinkProgram pfn_glLinkProgram
#define glGetProgramiv pfn_glGetProgramiv
#define glGetProgramInfoLog pfn_glGetProgramInfoLog
#define glUseProgram pfn_glUseProgram
#define glGetUniformLocation pfn_glGetUniformLocation
#define glUniform1i pfn_glUniform1i
#define glUniform4f pfn_glUniform4f
#define glUniformMatrix4fv pfn_glUniformMatrix4fv
#define glGenVertexArrays pfn_glGenVertexArrays
#define glBindVertexArray pfn_glBindVertexArray
#define glGenBuffers pfn_glGenBuffers
#define glBindBuffer pfn_glBindBuffer
#define glBufferData pfn_glBufferData
#define glEnableVertexAttribArray pfn_glEnableVertexAttribArray
#define glVertexAttribPointer pfn_glVertexAttribPointer
#define glDeleteShader pfn_glDeleteShader

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uMVP; out vec3 vPos; out vec2 vUV;\n"
    "void main(){ vPos = aPos; vUV = aUV; gl_Position = uMVP * vec4(aPos, 1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec3 vPos; in vec2 vUV; uniform sampler2D uTex; uniform int uMode; uniform vec4 uBox; out vec4 oCol;\n"
    "void main(){\n"
    "  vec2 tc = vUV;\n"
    "  if (uMode == 1) {                       /* projected plane: texel = where the arcade eye sees this point */\n"
    "    float zc = -vPos.z; float u = 144.0 + 346.0 * vPos.x / zc; float v = 112.0 - 346.0 * vPos.y / zc;\n"
    "    tc = vec2((u - uBox.x) / uBox.z, (v - uBox.y) / uBox.w);\n"
    "    if (tc.x < 0.0 || tc.x >= 1.0 || tc.y < 0.0 || tc.y >= 1.0) discard;\n"
    "  }\n"
    "  vec4 t = texture(uTex, tc);\n"
    "  if (t.a < 0.002) discard;\n"
    "  if (uMode == 2) oCol = vec4(0.5, 0.5, 0.5, 1.0); else oCol = vec4(t.rgb, 1.0);\n"
    "}\n";
static const char *VS2 =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV; uniform mat4 uMVP; out vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = uMVP * vec4(aPos, 1.0); }\n";
static const char *FS2 =
    "#version 330 core\n"
    "in vec2 vUV; uniform sampler2D uTex; out vec4 oCol;\n"
    "void main(){ oCol = texture(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "pbdesk: shader error: %s\n", log); exit(1); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, a); glAttachShader(p, b); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "pbdesk: link error: %s\n", log); exit(1); }
    glDeleteShader(a); glDeleteShader(b); return p;
}

/* ---------------------------------------------------------------- state source */
typedef struct { char ipc[512]; char file[512]; unsigned last_frame; int have; } Source;

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? len : 1); if (fread(b, 1, len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f); *n = len; return b;
}
/* returns a new state if the source has a newer frame than the last one returned (or NULL) */
static Nb1State *poll_state(Source *s, unsigned *frame) {
    char path[1100]; size_t n; uint8_t *b;
    if (s->file[0]) {
        if (s->have) return NULL;
        b = slurp(s->file, &n); if (!b) { fprintf(stderr, "pbdesk: cannot read %s\n", s->file); exit(1); }
        Nb1State *st = NULL; *frame = 0;
        if (n == NB1_STATE_FILE_BYTES) st = nb1_state_from_bytes(b, n);
        else if (n == NB1_STATE_FILE_BYTES + 12 && !memcmp(b, "PBS1", 4)) { st = nb1_state_from_bytes(b + 8, n - 12); *frame = (unsigned)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]); }
        free(b); if (!st) { fprintf(stderr, "pbdesk: %s is not a state file\n", s->file); exit(1); }
        s->have = 1; return st;
    }
    snprintf(path, sizeof path, "%s/latest.txt", s->ipc);
    b = slurp(path, &n); if (!b) return NULL;
    char name[256]; unsigned fr = 0; b = realloc(b, n + 1); b[n] = 0;
    if (sscanf((char *)b, "%255s %u", name, &fr) != 2) { free(b); return NULL; }
    free(b);
    if (s->have && fr == s->last_frame) return NULL;
    snprintf(path, sizeof path, "%s/%s", s->ipc, name);
    for (int tries = 0; tries < 3; tries++) {
        b = slurp(path, &n); if (!b) return NULL;
        if (n == NB1_STATE_FILE_BYTES + 12 && !memcmp(b, "PBS1", 4) && !memcmp(b + 4, b + n - 4, 4)) {   /* header frame == footer frame: not torn */
            Nb1State *st = nb1_state_from_bytes(b + 8, n - 12); *frame = (unsigned)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]);
            free(b); s->last_frame = *frame; s->have = 1; return st;
        }
        free(b); SDL_Delay(3);
    }
    return NULL;
}

/* ---------------------------------------------------------------- scene geometry for GL */
typedef struct { float x, y, z, u, v; } Vtx;
typedef struct { int first, count, mode; GLuint tex, shadow_tex; int box[4]; } Draw;

typedef struct {
    Nb1Scene scene; int32_t *ids; int loaded;
    Draw *draws; int ndraws; Vtx *verts; int nverts, capverts;
    int *order;                      /* item indices sorted by arcade order */
} GlScene;

static void push_vtx(GlScene *g, double x, double y, double z, double u, double v) {
    if (g->nverts == g->capverts) { g->capverts = g->capverts ? g->capverts * 2 : 4096; g->verts = realloc(g->verts, g->capverts * sizeof *g->verts); }
    g->verts[g->nverts++] = (Vtx){ (float)x, (float)y, (float)z, (float)u, (float)v };
}

/* clip a convex polygon (pixel coordinates) to g(x,y) = a*x + b*y + c >= 0 */
static int clip_poly(const double (*in)[2], int n, double a, double b, double c, double (*out)[2]) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const double *p = in[i], *q = in[(i + 1) % n];
        double gp = a * p[0] + b * p[1] + c, gq = a * q[0] + b * q[1] + c;
        if (gp >= 0) { out[m][0] = p[0]; out[m][1] = p[1]; m++; }
        if ((gp >= 0) != (gq >= 0)) { double t = gp / (gp - gq); out[m][0] = p[0] + t * (q[0] - p[0]); out[m][1] = p[1] + t * (q[1] - p[1]); m++; }
    }
    return m;
}

/* the polygon (world space) of an item; returns the vertex count (0 = nothing to draw) */
static int item_polygon(const Nb1Item *it, double far, double (*P)[3]) {
    if (!it->has_plane) {
        for (int k = 0; k < 3; k++) {
            P[0][k] = it->ctr[k] - it->r[k] + it->u[k]; P[1][k] = it->ctr[k] + it->r[k] + it->u[k];
            P[2][k] = it->ctr[k] + it->r[k] - it->u[k]; P[3][k] = it->ctr[k] - it->r[k] - it->u[k];
        }
        return 4;
    }
    double d = it->pd; if (fabs(d) < 1e-9) return 0;
    double box[4][2] = { { it->x0, it->y0 }, { it->x0 + it->w, it->y0 }, { it->x0 + it->w, it->y0 + it->h }, { it->x0, it->y0 + it->h } };
    double A[16][2], B[16][2]; int n = 4;
    for (int i = 0; i < 4; i++) { A[i][0] = box[i][0]; A[i][1] = box[i][1]; }
    double s = d > 0 ? 1 : -1, cx = NB1_W / 2.0, cy = NB1_H / 2.0, F = NB1_FOCAL;
    /* den = n . ray(x, y) = nx*(x-cx)/F + ny*(cy-y)/F - nz  (linear in x, y); depth = d/den must be in (0.05, far] */
    double ax = it->pn[0] / F, ay = -it->pn[1] / F, ac = -it->pn[0] * cx / F + it->pn[1] * cy / F - it->pn[2];
    n = clip_poly((const double (*)[2])A, n, s * ax, s * ay, s * ac - fabs(d) / far, B);          /* depth <= far (and den has the sign of d) */
    if (n < 3) return 0;
    n = clip_poly((const double (*)[2])B, n, -s * ax, -s * ay, -s * ac + fabs(d) / 0.05, A);       /* depth >= 0.05 */
    if (n < 3) return 0;
    for (int i = 0; i < n; i++) {
        double x = A[i][0], y = A[i][1], ray[3] = { (x - cx) / F, (cy - y) / F, -1.0 };
        double den = it->pn[0] * ray[0] + it->pn[1] * ray[1] + it->pn[2] * ray[2], q = d / den;
        for (int k = 0; k < 3; k++) P[i][k] = ray[k] * q;
    }
    return n;
}

static GLuint make_tex(const uint8_t *rgba, int w, int h) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void gl_scene_free(GlScene *g) {
    for (int i = 0; i < g->ndraws; i++) { if (g->draws[i].tex) glDeleteTextures(1, &g->draws[i].tex); if (g->draws[i].shadow_tex) glDeleteTextures(1, &g->draws[i].shadow_tex); }
    free(g->draws); free(g->verts); free(g->order); free(g->ids);
    if (g->loaded) nb1_scene_free(&g->scene);
    memset(g, 0, sizeof *g);
}

static void gl_scene_build(GlScene *g, const Nb1State *st, const Nb1Roms *roms, const Nb1Table *tab) {
    gl_scene_free(g);
    nb1_build_scene(st, st, roms, tab, &g->scene); g->loaded = 1;
    g->ids = malloc(NB1_W * NB1_H * 4); nb1_visible_map(&g->scene, g->ids);
    int n = g->scene.n; g->draws = calloc(n, sizeof *g->draws); g->order = malloc(n * sizeof *g->order);
    for (int i = 0; i < n; i++) g->order[i] = i;
    for (int i = 1; i < n; i++) { int v = g->order[i], j = i - 1; while (j >= 0 && g->scene.items[g->order[j]].order > g->scene.items[v].order) { g->order[j + 1] = g->order[j]; j--; } g->order[j + 1] = v; }
    g->ndraws = n;
    for (int i = 0; i < n; i++) {
        const Nb1Item *it = &g->scene.items[i]; Draw *d = &g->draws[i];
        double P[16][3]; int m = item_polygon(it, g->scene.far, P);
        d->first = g->nverts; d->count = 0; d->mode = it->has_plane ? 1 : 0;
        d->box[0] = it->x0; d->box[1] = it->y0; d->box[2] = it->w; d->box[3] = it->h;
        if (m >= 3) {
            static const double uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            for (int k = 1; k + 1 < m; k++) {
                int idx[3] = { 0, k, k + 1 };
                for (int q = 0; q < 3; q++) { int v = idx[q]; push_vtx(g, P[v][0], P[v][1], P[v][2], m == 4 ? uv[v][0] : 0, m == 4 ? uv[v][1] : 0); }
            }
            d->count = (m - 2) * 3;
        }
        d->tex = make_tex(it->tex, it->w, it->h);
        if (it->shadow) {                                          /* mask of the shadow pens (pen -2) */
            size_t N = (size_t)it->w * it->h; uint8_t *mk = calloc(N, 4);
            for (size_t k = 0; k < N; k++) if (it->pen[k] == -2) mk[k * 4 + 3] = 255;
            d->shadow_tex = make_tex(mk, it->w, it->h); free(mk);
        }
    }
}

/* ---------------------------------------------------------------- matrices */
static void make_mvp(const Nb1Cam *c, int vw, int vh, float *m /* column-major */) {
    double n = 0.3, f = 400.0, P00 = 2.0 * c->f / vw, P11 = 2.0 * c->f / vh, P22 = -(f + n) / (f - n), P23 = -2.0 * f * n / (f - n);
    /* camera space: x = rel.right, y = rel.up, z = -(rel.fwd) */
    double V[4][4] = { { c->right[0], c->right[1], c->right[2], -(c->right[0] * c->E[0] + c->right[1] * c->E[1] + c->right[2] * c->E[2]) },
                       { c->up[0], c->up[1], c->up[2], -(c->up[0] * c->E[0] + c->up[1] * c->E[1] + c->up[2] * c->E[2]) },
                       { -c->fwd[0], -c->fwd[1], -c->fwd[2], (c->fwd[0] * c->E[0] + c->fwd[1] * c->E[1] + c->fwd[2] * c->E[2]) },
                       { 0, 0, 0, 1 } };
    double Pm[4][4] = { { P00, 0, 0, 0 }, { 0, P11, 0, 0 }, { 0, 0, P22, P23 }, { 0, 0, -1, 0 } };
    for (int r = 0; r < 4; r++) for (int col = 0; col < 4; col++) {
        double s = 0; for (int k = 0; k < 4; k++) s += Pm[r][k] * V[k][col];
        m[col * 4 + r] = (float)s;
    }
}

/* ---------------------------------------------------------------- text overlay (CPU-composed strip) */
static void put_text(uint8_t *buf, int bw, int bh, int x, int y, const char *s) {
    for (; *s; s++, x += 7) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) continue;
        for (int r = 0; r < 13; r++) for (int b = 0; b < 7; b++) {
            if (!(FONT7X13[c - 32][r] & (1 << (6 - b)))) continue;
            int px = x + b, py = y + r; if (px < 0 || px >= bw || py < 0 || py >= bh) continue;
            uint8_t *q = buf + ((size_t)py * bw + px) * 4; q[0] = q[1] = q[2] = 255; q[3] = 255;
        }
    }
}

/* ---------------------------------------------------------------- app */
typedef struct {
    char roms[512], ipc[512], statefile[512], table[512], shot[512];
    int w, h, mode, hide_hud, use_table, paused, shot_after, show_help, no_text, shot_game_frame, frames_limit, mouse_x, mouse_y, mouse_set, trigger_set;
    double yaw, pitch, dist, focus, focal;
} Opts;

static void die_usage(void) {
    fprintf(stderr, "usage: pbdesk --roms DIR (--ipc DIR | --state FILE) [--table T.json] [--res WxH] [--yaw N] [--pitch N] [--dist N]\n"
                    "              [--focus N] [--focal N] [--hide-hud] [--no-text] [--shot out.ppm [--shot-after N | --shot-game-frame N]]\n");
    exit(2);
}

static void save_ppm(const char *path, int w, int h) {
    uint8_t *px = malloc((size_t)w * h * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); free(px); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    fclose(f); free(px); fprintf(stderr, "saved %s\n", path);
}

int main(int argc, char **argv) {
    Opts o; memset(&o, 0, sizeof o); o.w = 960; o.h = 600; o.mode = 1; o.use_table = 1; o.pitch = 14; o.dist = 15; o.focus = 9; o.focal = 620; o.shot_after = 8;
    int yaw_given = 0; o.yaw = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--roms") && v) { snprintf(o.roms, sizeof o.roms, "%s", v); i++; }
        else if (!strcmp(a, "--ipc") && v) { snprintf(o.ipc, sizeof o.ipc, "%s", v); i++; }
        else if (!strcmp(a, "--state") && v) { snprintf(o.statefile, sizeof o.statefile, "%s", v); i++; }
        else if (!strcmp(a, "--table") && v) { snprintf(o.table, sizeof o.table, "%s", v); i++; }
        else if (!strcmp(a, "--res") && v) { if (sscanf(v, "%dx%d", &o.w, &o.h) != 2) die_usage(); i++; }
        else if (!strcmp(a, "--yaw") && v) { o.yaw = atof(v); yaw_given = 1; o.mode = 2; i++; }
        else if (!strcmp(a, "--pitch") && v) { o.pitch = atof(v); i++; }
        else if (!strcmp(a, "--dist") && v) { o.dist = atof(v); i++; }
        else if (!strcmp(a, "--focus") && v) { o.focus = atof(v); i++; }
        else if (!strcmp(a, "--focal") && v) { o.focal = atof(v); i++; }
        else if (!strcmp(a, "--hide-hud")) o.hide_hud = 1;
        else if (!strcmp(a, "--no-text")) o.no_text = 1;
        else if (!strcmp(a, "--shot-game-frame") && v) { o.shot_game_frame = atoi(v); i++; }                 /* screenshot once the game reaches this frame */
        else if (!strcmp(a, "--frames") && v) { o.frames_limit = atoi(v); i++; }                       /* test: quit after N frames */
        else if (!strcmp(a, "--mouse") && v) { if (sscanf(v, "%d,%d", &o.mouse_x, &o.mouse_y) != 2) die_usage(); o.mouse_set = 1; i++; }   /* test: start with the mouse here */
        else if (!strcmp(a, "--trigger")) o.trigger_set = 1;                                            /* test: trigger held */
        else if (!strcmp(a, "--shot") && v) { snprintf(o.shot, sizeof o.shot, "%s", v); i++; }
        else if (!strcmp(a, "--shot-after") && v) { o.shot_after = atoi(v); i++; }
        else die_usage();
    }
    (void)yaw_given;
    if (!o.roms[0] || (!o.ipc[0] && !o.statefile[0])) die_usage();

    Nb1Roms roms; if (nb1_roms_load(&roms, o.roms)) { fprintf(stderr, "pbdesk: cannot load ROMs from %s\n", o.roms); return 1; }
    Nb1Table *tab = o.table[0] ? nb1_table_load(o.table) : NULL;
    if (o.table[0] && !tab) { fprintf(stderr, "pbdesk: cannot read table %s (using the default)\n", o.table); }
    Nb1Table *deftab = nb1_table_default();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) { fprintf(stderr, "pbdesk: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("Point Blank depth viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, o.w, o.h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "pbdesk: window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "pbdesk: OpenGL 3.3 context: %s\n", SDL_GetError()); return 1; }
    if (load_gl()) return 1;
    SDL_GL_SetSwapInterval(o.shot[0] ? 0 : 1);
    GLuint prog = program(VS, FS), prog2 = program(VS2, FS2);
    GLint uMVP = glGetUniformLocation(prog, "uMVP"), uTex = glGetUniformLocation(prog, "uTex"), uMode = glGetUniformLocation(prog, "uMode"), uBox = glGetUniformLocation(prog, "uBox");
    GLint uMVP2 = glGetUniformLocation(prog2, "uMVP"), uTex2 = glGetUniformLocation(prog2, "uTex");
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(3 * sizeof(float)));
    GLuint txt_tex = 0; glGenTextures(1, &txt_tex);
    GLuint vao2, vbo2; glGenVertexArrays(1, &vao2); glBindVertexArray(vao2); glGenBuffers(1, &vbo2); glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(3 * sizeof(float)));
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);

    Source src; memset(&src, 0, sizeof src); snprintf(src.ipc, sizeof src.ipc, "%s", o.ipc); snprintf(src.file, sizeof src.file, "%s", o.statefile);
    GlScene gs; memset(&gs, 0, sizeof gs);
    Nb1State *cur = NULL; unsigned game_frame = 0, shown_frame = 0; int dirty = 0;
    int trig = 0, coin = 0, start = 0, seq = 0, running = 1, drag = 0, rendered = 0; int mx = 0, my = 0; double sx = 100, sy = 100;
    char aim_txt[256] = ""; double build_ms = 0;
    if (o.mouse_set) { mx = o.mouse_x; my = o.mouse_y; }
    if (o.trigger_set) trig = 1;
    char aim_path[1100], aim_tmp[1110]; snprintf(aim_path, sizeof aim_path, "%s/aim.txt", o.ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s.tmp", aim_path);
    Uint64 t_prev = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_1: o.mode = 1; break;
                case SDLK_2: o.mode = 2; break;
                case SDLK_h: o.hide_hud = !o.hide_hud; break;
                case SDLK_t: o.use_table = !o.use_table; dirty = 1; break;
                case SDLK_r: if (o.table[0]) { Nb1Table *nt = nb1_table_load(o.table); if (nt) { nb1_table_free(tab); tab = nt; dirty = 1; } else fprintf(stderr, "pbdesk: table reload failed\n"); } break;
                case SDLK_p: o.paused = !o.paused; break;
                case SDLK_F1: o.show_help = !o.show_help; break;
                case SDLK_F5: { char fn[64]; snprintf(fn, sizeof fn, "pbdesk_%ld.ppm", (long)time(NULL)); o.shot_after = -1; snprintf(o.shot, sizeof o.shot, "%s", fn); break; }
                case SDLK_c: coin = 1; break;
                case SDLK_v: start = 1; break;
                default: break;
                }
            } else if (ev.type == SDL_KEYUP) { if (ev.key.keysym.sym == SDLK_c) coin = 0; if (ev.key.keysym.sym == SDLK_v) start = 0; }
            else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (ev.button.button == SDL_BUTTON_LEFT) { if (SDL_GetModState() & KMOD_ALT) drag = 1; else trig = 1; }
                if (ev.button.button == SDL_BUTTON_RIGHT) drag = 1;
            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                if (ev.button.button == SDL_BUTTON_LEFT) { trig = 0; drag = 0; }
                if (ev.button.button == SDL_BUTTON_RIGHT) drag = 0;
            } else if (ev.type == SDL_MOUSEMOTION) {
                if (!o.mouse_set) { mx = ev.motion.x; my = ev.motion.y; }       /* --mouse pins the pointer (tests) */
                if (drag) { o.yaw += ev.motion.xrel * 0.3; o.pitch += ev.motion.yrel * 0.3; if (o.pitch > 85) o.pitch = 85; if (o.pitch < -85) o.pitch = -85; o.mode = 2; }
            } else if (ev.type == SDL_MOUSEWHEEL) { o.dist -= ev.wheel.y * 0.8; if (o.dist < 2) o.dist = 2; }
        }
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_A]) o.yaw -= 1.2;
        if (keys[SDL_SCANCODE_D]) o.yaw += 1.2;
        if (keys[SDL_SCANCODE_W]) { o.pitch += 0.8; if (o.pitch > 85) o.pitch = 85; }
        if (keys[SDL_SCANCODE_S]) { o.pitch -= 0.8; if (o.pitch < -85) o.pitch = -85; }
        if (keys[SDL_SCANCODE_Q]) { o.dist -= 0.15; if (o.dist < 2) o.dist = 2; }
        if (keys[SDL_SCANCODE_E]) o.dist += 0.15;
        if (keys[SDL_SCANCODE_Z]) { o.focus -= 0.1; if (o.focus < 1) o.focus = 1; }
        if (keys[SDL_SCANCODE_X]) o.focus += 0.1;

        /* new game state? */
        if (!o.paused) { unsigned fr = 0; Nb1State *ns = poll_state(&src, &fr); if (ns) { free(cur); cur = ns; game_frame = fr; dirty = 1; } }
        if (dirty && cur) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            gl_scene_build(&gs, cur, &roms, (o.use_table && tab) ? tab : deftab);
            build_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)gs.nverts * sizeof(Vtx), gs.verts, GL_DYNAMIC_DRAW);
            shown_frame = game_frame; dirty = 0;
        }

        int ww, wh; SDL_GL_GetDrawableSize(win, &ww, &wh);
        int sw, sh_; SDL_GetWindowSize(win, &sw, &sh_);
        double pxscale = sw > 0 ? (double)ww / sw : 1.0;

        /* camera + viewport */
        Nb1Cam cam; int vx = 0, vy = 0, vw = ww, vh = wh;
        if (o.mode == 1) {
            double sc = fmin((double)ww / NB1_W, (double)wh / NB1_H); nb1_arcade_camera(&cam, 1);
            cam.f = NB1_FOCAL * sc; vw = (int)(NB1_W * sc); vh = (int)(NB1_H * sc); vx = (ww - vw) / 2; vy = (wh - vh) / 2;
        } else {
            const double target[3] = { 0, 0, -o.focus }; nb1_orbit_camera(target, o.dist, o.yaw, o.pitch, &cam); cam.f = o.focal * ww / 960.0;
        }
        cam.w = vw; cam.h = vh;

        /* aim: the mouse ray through the scene that is on screen */
        if (gs.loaded) {
            double u = mx * pxscale + 0.5 - vx, v = my * pxscale + 0.5 - vy, d[3];
            double a = (u - vw / 2.0) / cam.f, b = (vh / 2.0 - v) / cam.f;
            for (int k = 0; k < 3; k++) d[k] = a * cam.right[k] + b * cam.up[k] + cam.fwd[k];
            Nb1AimSnap r;
            if (!nb1_aim_snap(&gs.scene, gs.ids, cam.E, d, 24.0, &r))
                snprintf(aim_txt, sizeof aim_txt, "aim: no hit, keeping last pixel (%5.1f,%5.1f)   [scene frame %u]", sx, sy, shown_frame);
            else {
                const Nb1Item *it = &gs.scene.items[r.a.item];
                const char *kn = it->kind == NB1_TILEMAP ? "tilemap" : it->kind == NB1_SPRITE ? "sprite" : "mask";
                if (r.hidden) {
                    sx = r.a.sx; sy = r.a.sy;
                    const Nb1Item *cv = r.cover >= 0 ? &gs.scene.items[r.cover] : NULL;
                    snprintf(aim_txt, sizeof aim_txt, "aim HIDDEN: %s%d z=%.1f is covered here; shot lands on %s%d (%5.1f,%5.1f)", kn, it->id, it->z,
                             cv ? (cv->kind == NB1_TILEMAP ? "tilemap" : "sprite") : "nothing", cv ? cv->id : 0, sx, sy);
                } else {
                    sx = r.a.sx; sy = r.a.sy;
                    snprintf(aim_txt, sizeof aim_txt, "aim px (%5.1f,%5.1f) hits %s%d%s%s z=%.1f%s   [scene frame %u]", sx, sy, kn, it->id, it->has_part ? ":" : "", it->has_part ? it->part : "", it->z,
                             r.snapped ? "  SNAPPED" : "", shown_frame);
                }
            }
        }
        if (o.ipc[0]) {                                           /* publish the aim for the game */
            FILE *f = fopen(aim_tmp, "w");
            if (f) { fprintf(f, "%d %.2f %.2f %d %d %d\n", ++seq, sx, sy, trig, coin, start); fclose(f); rename(aim_tmp, aim_path); }
        }

        /* draw */
        glViewport(0, 0, ww, wh); glClearColor(0.08f, 0.08f, 0.09f, 1); glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (gs.loaded) {
            glViewport(vx, vy, vw, vh); glEnable(GL_SCISSOR_TEST); glScissor(vx, vy, vw, vh);
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            float mvp[16]; make_mvp(&cam, vw, vh, mvp);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
            glUseProgram(prog); glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp); glUniform1i(uTex, 0); glBindVertexArray(vao);
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_SRC_COLOR); glDepthFunc(GL_LESS); glDepthMask(GL_FALSE); }
                for (int oi = 0; oi < gs.ndraws; oi++) {
                    int i = gs.order[oi]; const Nb1Item *it = &gs.scene.items[i]; const Draw *d = &gs.draws[i];
                    if (!d->count || (o.hide_hud && it->hud)) continue;
                    if (pass == 1 && !d->shadow_tex) continue;
                    glBindTexture(GL_TEXTURE_2D, pass ? d->shadow_tex : d->tex);
                    glUniform1i(uMode, pass ? 2 : d->mode); glUniform4f(uBox, (float)d->box[0], (float)d->box[1], (float)d->box[2], (float)d->box[3]);
                    glDrawArrays(GL_TRIANGLES, d->first, d->count);
                }
            }
            glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDisable(GL_SCISSOR_TEST);
        }
        /* status text */
        if (!o.no_text) {
            char l1[256], l2[640]; const char *tn = !o.use_table ? "off (auto depth)" : (o.table[0] ? strrchr(o.table, '/') ? strrchr(o.table, '/') + 1 : o.table : "default");
            snprintf(l1, sizeof l1, "%s  yaw %.1f pitch %.1f dist %.1f focus %.1f   game frame %u  build %.0f ms%s", o.mode == 1 ? "ARCADE" : "ORBIT", o.yaw, o.pitch, o.dist, o.focus, shown_frame, build_ms, o.paused ? "  PAUSED" : "");
            snprintf(l2, sizeof l2, "table: %s   F1 help", tn);
            const char *help[] = { "1 arcade view  2 orbit  A/D yaw  W/S pitch  Q/E distance  Z/X focus  right-drag or Alt+drag orbit  wheel distance",
                                   "H hide HUD  T table on/off  R reload table  P pause  F5 screenshot  ESC quit   C coin  V start  left mouse = trigger" };
            int lines = 3 + (o.show_help ? 2 : 0), bh = lines * 15 + 4, bw = ww < 1100 ? ww : 1100; uint8_t *buf = calloc((size_t)bw * bh, 4);
            for (size_t k = 0; k < (size_t)bw * bh; k++) { buf[k * 4 + 3] = 150; }
            put_text(buf, bw, bh, 4, 2, l1); put_text(buf, bw, bh, 4, 17, aim_txt); put_text(buf, bw, bh, 4, 32, l2);
            if (o.show_help) { put_text(buf, bw, bh, 4, 47, help[0]); put_text(buf, bw, bh, 4, 62, help[1]); }
            glViewport(0, 0, ww, wh); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, txt_tex); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            float x1 = 2.0f * bw / ww - 1.0f, y1 = 1.0f - 2.0f * bh / wh;
            Vtx quad[6] = { { -1, 1, 0, 0, 0 }, { x1, 1, 0, 1, 0 }, { x1, y1, 0, 1, 1 }, { -1, 1, 0, 0, 0 }, { x1, y1, 0, 1, 1 }, { -1, y1, 0, 0, 1 } };
            static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            glBindVertexArray(vao2); glBindBuffer(GL_ARRAY_BUFFER, vbo2); glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
            glUseProgram(prog2); glUniformMatrix4fv(uMVP2, 1, GL_FALSE, ident); glUniform1i(uTex2, 0); glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glDisable(GL_BLEND); free(buf);
            SDL_SetWindowTitle(win, aim_txt[0] ? aim_txt : "Point Blank depth viewer");
        }
        rendered++;
        if (o.frames_limit && rendered >= o.frames_limit) running = 0;
        if (o.shot[0] && (o.shot_after < 0 || (o.shot_game_frame ? (int)shown_frame >= o.shot_game_frame : rendered >= o.shot_after)) && gs.loaded) {
            save_ppm(o.shot, ww, wh);
            if (o.shot_after >= 0) running = 0; else o.shot[0] = 0;
        }
        SDL_GL_SwapWindow(win);
        Uint64 now = SDL_GetPerformanceCounter(); (void)t_prev; t_prev = now;
        if (o.shot[0]) SDL_Delay(2);
    }
    gl_scene_free(&gs); free(cur); nb1_table_free(tab); nb1_table_free(deftab); nb1_roms_free(&roms);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
