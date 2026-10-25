/* Shared by pbdesk (desktop window) and pbvr (OpenXR): GL loading, scene -> GL geometry/textures, rulers, matrices, state source. */
#pragma once
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
#include <windows.h>
#undef far                                                           /* windows.h's segment-era macros collide with names like `far` */
#undef near
/* rename() refuses to replace an existing file on Windows; the aim file is replaced atomically instead */
static int replace_file(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1; }
#else
#define replace_file rename
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
    "  if (uMode == 3) { oCol = vec4(uBox.rgb, 1.0); return; }        /* ruler lines */\n"
    "  vec2 tc = vUV;\n"
    "  if (uMode == 1) {                       /* projected plane: texel = where the arcade eye sees this point */\n"
    "    float zc = -vPos.z; float u = 144.0 + 346.0 * vPos.x / zc; float v = 112.0 - 346.0 * vPos.y / zc;\n"
    "    tc = vec2((u - uBox.x) / uBox.z, (v - uBox.y) / uBox.w);\n"
    "    if (tc.x < 0.0 || tc.x >= 1.0 || tc.y < 0.0 || tc.y >= 1.0) discard;\n"
    "  }\n"
    "  vec4 t = texture(uTex, tc);\n"
    "  if (t.a < 0.002) discard;\n"
    "  if (uMode == 4) { if (t.a < 0.5) discard; oCol = vec4(t.rgb, 1.0); return; }   /* ruler labels */\n"
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

typedef struct { int first, count, hud; float col[3]; } Ruler;                       /* a ruler = a run of GL_LINES vertices */
typedef struct { double pos[3]; int right_align, hud, ruler; GLuint tex; int tw, th; float tint[3]; } Label;   /* billboard text anchored in the world */

typedef struct {
    Nb1Scene scene; int32_t *ids; int loaded;
    Draw *draws; int ndraws; Vtx *verts; int nverts, capverts;
    int *order;                      /* item indices sorted by arcade order */
    Ruler *rulers; int nrulers; Vtx *rverts; int nrverts, caprverts; Label *labels; int nlabels; double zs;   /* metre rulers beside each depth layer */
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

/* ---------------------------------------------------------------- metre rulers beside each depth layer */
static void put_text_c(uint8_t *buf, int bw, int bh, int x, int y, const char *s, int r, int g, int b) {
    for (; *s; s++, x += 7) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) continue;
        for (int row = 0; row < 13; row++) for (int bit = 0; bit < 7; bit++) {
            if (!(FONT7X13[c - 32][row] & (1 << (6 - bit)))) continue;
            int px = x + bit, py = y + row; if (px < 0 || px >= bw || py < 0 || py >= bh) continue;
            uint8_t *q = buf + ((size_t)py * bw + px) * 4; q[0] = (uint8_t)r; q[1] = (uint8_t)g; q[2] = (uint8_t)b; q[3] = 255;
        }
    }
}
static GLuint text_tex(const char *s, int *tw, int *th) {            /* white text with a black outline, on a transparent background */
    int n = (int)strlen(s), w = n * 7 + 4, h = 17; uint8_t *buf = calloc((size_t)w * h, 4);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) put_text_c(buf, w, h, 2 + dx, 2 + dy, s, 0, 0, 0);
    put_text_c(buf, w, h, 2, 2, s, 255, 255, 255);
    GLuint t = make_tex(buf, w, h); free(buf); *tw = w; *th = h; return t;
}
static void push_rvert(GlScene *g, double x, double y, double z) {
    if (g->nrverts == g->caprverts) { g->caprverts = g->caprverts ? g->caprverts * 2 : 2048; g->rverts = realloc(g->rverts, g->caprverts * sizeof *g->rverts); }
    g->rverts[g->nrverts++] = (Vtx){ (float)x, (float)y, (float)z, 0, 0 };
}
static void add_label(GlScene *g, double x, double y, double z, const char *text, int right_align, int hud, const float *tint) {
    g->labels = realloc(g->labels, (g->nlabels + 1) * sizeof *g->labels); Label *l = &g->labels[g->nlabels++]; l->ruler = g->nrulers - 1;
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; l->right_align = right_align; l->hud = hud; l->tex = text_tex(text, &l->tw, &l->th); memcpy(l->tint, tint, sizeof l->tint);
}

typedef struct { int key, zb, n, hud, tilemap, id, pri; double zsum, xmin, ymin, ymax; long area; } Grp;

static void build_rulers(GlScene *g) {
    Grp grp[256]; int ng = 0;
    for (int i = 0; i < g->scene.n; i++) {
        const Nb1Item *it = &g->scene.items[i]; if (it->kind == NB1_MASK || it->decal || it->has_plane) continue;
        double P[16][3]; if (item_polygon(it, g->scene.far, P) != 4) continue;
        double xmin = 1e30, ymin = 1e30, ymax = -1e30, z = 0;
        for (int k = 0; k < 4; k++) {
            if (P[k][0] < xmin) xmin = P[k][0];
            if (P[k][1] < ymin) ymin = P[k][1];
            if (P[k][1] > ymax) ymax = P[k][1];
            z += -P[k][2] / 4;
        }
        int tm = it->kind == NB1_TILEMAP, key = tm ? it->id : 100 + it->pri, zb = (int)floor(z / 0.25 + 0.5), f = -1;
        for (int q = 0; q < ng; q++) if (grp[q].key == key && grp[q].zb == zb) { f = q; break; }
        if (f < 0) {
            if (ng == 256) continue;
            f = ng++; grp[f] = (Grp){ key, zb, 0, 1, tm, tm ? it->id : 0, it->pri, 0, xmin, ymin, ymax, 0 };
        }
        Grp *q = &grp[f]; q->n++; q->zsum += z; q->area += (long)it->w * it->h; q->hud &= it->hud;
        if (xmin < q->xmin) q->xmin = xmin;
        if (ymin < q->ymin) q->ymin = ymin;
        if (ymax > q->ymax) q->ymax = ymax;
    }
    int idx[256], ni = 0;                                              /* tilemap layers first (nearest last), then the biggest sprite groups */
    for (int q = 0; q < ng; q++) if (grp[q].ymax - grp[q].ymin >= 0.3) idx[ni++] = q;
    for (int a = 1; a < ni; a++) { int v = idx[a], b = a - 1;
        while (b >= 0 && ((grp[idx[b]].tilemap < grp[v].tilemap) || (grp[idx[b]].tilemap == grp[v].tilemap && (grp[v].tilemap ? grp[idx[b]].zsum / grp[idx[b]].n < grp[v].zsum / grp[v].n : grp[idx[b]].area < grp[v].area)))) { idx[b + 1] = idx[b]; b--; }
        idx[b + 1] = v; }
    if (ni > 24) ni = 24;
    double zmin = 1e30, zmax = -1e30;
    for (int a = 0; a < ni; a++) { double z = grp[idx[a]].zsum / grp[idx[a]].n; if (z < zmin) zmin = z; if (z > zmax) zmax = z; }
    g->rulers = calloc((size_t)(ni > 0 ? ni : 1), sizeof *g->rulers);
    for (int a = 0; a < ni; a++) {
        const Grp *q = &grp[idx[a]]; double z = q->zsum / q->n, H = q->ymax - q->ymin, x = q->xmin - 0.25;
        int stack = 0; for (int b = 0; b < a; b++) if (fabs(grp[idx[b]].xmin - q->xmin) < 0.6) stack++;       /* rulers sharing a left edge step outwards */
        x -= 0.22 * stack;
        double k = zmax > zmin ? (z - zmin) / (zmax - zmin) : 0.0;                                     /* near = red, far = cyan */
        Ruler *r = &g->rulers[g->nrulers++]; r->hud = q->hud; r->col[0] = (float)(1.0 - 0.7 * k); r->col[1] = (float)(0.35 + 0.55 * k); r->col[2] = (float)(0.3 + 0.7 * k);
        r->first = g->nrverts;
        int coarse = H > 12.0; double step = coarse ? 1.0 : 0.25;
        push_rvert(g, x, q->ymin, -z); push_rvert(g, x, q->ymax, -z);                                   /* the spine, up to the layer height */
        for (int t = 0; t * step <= H + 1e-9; t++) {
            double v = t * step, len = (fabs(v - floor(v + 0.5)) < 1e-9) ? 0.32 : (fabs(v * 2 - floor(v * 2 + 0.5)) < 1e-9 ? 0.22 : 0.12);
            push_rvert(g, x, q->ymin + v, -z); push_rvert(g, x + len, q->ymin + v, -z);
            int whole = fabs(v - floor(v + 0.5)) < 1e-9, m = (int)floor(v + 0.5);
            if (whole && m > 0 && (!coarse || m % 5 == 0)) { char b[16]; snprintf(b, sizeof b, "%dm", m); add_label(g, x - 0.04, q->ymin + v, -z, b, 1, r->hud, r->col); }
        }
        r->count = g->nrverts - r->first;
        char name[40], text[80]; if (q->tilemap) snprintf(name, sizeof name, "L%d", q->id); else snprintf(name, sizeof name, "S%d x%d", q->pri, q->n);
        snprintf(text, sizeof text, "%s  z=%.1fm  h=%.1fm", name, z * g->zs, H); add_label(g, x, q->ymax + 0.12 + 0.34 * stack, -z, text, 0, r->hud, r->col);
    }
}

static void gl_scene_free(GlScene *g) {
    for (int i = 0; i < g->nlabels; i++) glDeleteTextures(1, &g->labels[i].tex);
    free(g->labels); free(g->rulers); free(g->rverts);
    for (int i = 0; i < g->ndraws; i++) { if (g->draws[i].tex) glDeleteTextures(1, &g->draws[i].tex); if (g->draws[i].shadow_tex) glDeleteTextures(1, &g->draws[i].shadow_tex); }
    free(g->draws); free(g->verts); free(g->order); free(g->ids);
    if (g->loaded) nb1_scene_free(&g->scene);
    { double zs_ = g->zs; memset(g, 0, sizeof *g); g->zs = zs_; }
}

/* CPU half of a scene build (no GL calls, so a worker thread may run it); gl_scene_upload() does the rest on the GL thread */
static void gl_scene_cpu(GlScene *g, const Nb1State *st, const Nb1Roms *roms, const Nb1Table *tab) {
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
    }
}
static void gl_scene_upload(GlScene *g) {
    for (int i = 0; i < g->scene.n; i++) {
        const Nb1Item *it = &g->scene.items[i]; Draw *d = &g->draws[i];
        d->tex = make_tex(it->tex, it->w, it->h);
        if (it->shadow) {                                          /* mask of the shadow pens (pen -2) */
            size_t N = (size_t)it->w * it->h; uint8_t *mk = calloc(N, 4);
            for (size_t k = 0; k < N; k++) if (it->pen[k] == -2) mk[k * 4 + 3] = 255;
            d->shadow_tex = make_tex(mk, it->w, it->h); free(mk);
        }
    }
    build_rulers(g);
}
static void gl_scene_build(GlScene *g, const Nb1State *st, const Nb1Roms *roms, const Nb1Table *tab) { gl_scene_cpu(g, st, roms, tab); gl_scene_upload(g); }

/* ---------------------------------------------------------------- matrices */
static void make_mvp(const Nb1Cam *c, int vw, int vh, double zs, float *m /* column-major */) {
    double n = 0.3, f = 400.0, P00 = 2.0 * c->f / vw, P11 = 2.0 * c->f / vh, P22 = -(f + n) / (f - n), P23 = -2.0 * f * n / (f - n);
    /* camera space: x = rel.right, y = rel.up, z = -(rel.fwd) */
    double V[4][4] = { { c->right[0], c->right[1], c->right[2], -(c->right[0] * c->E[0] + c->right[1] * c->E[1] + c->right[2] * c->E[2]) },
                       { c->up[0], c->up[1], c->up[2], -(c->up[0] * c->E[0] + c->up[1] * c->E[1] + c->up[2] * c->E[2]) },
                       { -c->fwd[0], -c->fwd[1], -c->fwd[2], (c->fwd[0] * c->E[0] + c->fwd[1] * c->E[1] + c->fwd[2] * c->E[2]) },
                       { 0, 0, 0, 1 } };
    /* model matrix diag(1,1,zs): the scene is authored for the arcade eye (f = 346 px); a physical viewport wants the same sizes at zs times the depth */
    for (int r = 0; r < 3; r++) V[r][2] *= zs;
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

