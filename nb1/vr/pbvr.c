/* pbvr: PC VR host for the NB-1 depth scene (OpenXR + OpenGL 3.3, SDL2 for the window and GL context).
 *
 *   pbvr --roms DIR --ipc DIR [--tables scene_tables.json | --table T.json] [--screen-diag IN] [--screen-dist M] [--viewport-scale K] [--gun-pitch DEG] [--no-vr]
 *
 * MAME runs the game (tools/play/pbplay.lua) and publishes state files into --ipc; pbvr builds the depth scene from each one, draws it in
 * stereo and writes aim.txt (arcade pixel from the controller ray, trigger, coin, start, pause) back for MAME. It is the VR counterpart of
 * pbdesk and shares glscene.h with it.
 *
 * Space: the arcade eye is wherever your head is when you recenter (start, or the right stick click); the picture is a window of
 * --screen-diag inches at --screen-dist metres in front of that point (default 29" at 0.9 m, as pbdesk), and layers sit behind it with their
 * authored sizes (depth scale = f_eff / 346, see the README).
 *
 * Controls (Touch layout; Index and simple controllers are bound too): trigger = fire (the hand that pulled it last is the gun),
 *   A / X = coin   B / Y = start   left stick click = pause the game   right stick click = recenter.
 * Keyboard (window focused): C coin  V start  P pause  R recenter  [ ] viewport size  ESC quit.
 * The OpenXR loader (openxr_loader.dll / libopenxr_loader.so.1) is loaded at run time, so the program starts, and says what is missing,
 * without it.
 */
#define XR_NO_PROTOTYPES
#include "../desktop/glscene.h"
#ifdef _WIN32
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#else
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* ------------------------------------------------------------------ OpenXR function table (loaded from the loader) */
#define XR_FUNCS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) \
    X(xrCreateReferenceSpace) X(xrCreateActionSpace) X(xrDestroySpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrDestroySwapchain) \
    X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
    X(xrLocateSpace) X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateFloat) X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) X(xrResultToString)
#define X(N) static PFN_##N N;
XR_FUNCS(X)
#undef X
static PFN_xrGetOpenGLGraphicsRequirementsKHR pxrGetOpenGLGraphicsRequirementsKHR;

static int load_openxr(XrInstance inst, PFN_xrGetInstanceProcAddr gipa) {
#define X(N) if (XR_FAILED(gipa(inst, #N, (PFN_xrVoidFunction *)&N))) { fprintf(stderr, "pbvr: OpenXR loader lacks %s\n", #N); return -1; }
    XR_FUNCS(X)
#undef X
    return 0;
}
static PFN_xrGetInstanceProcAddr open_loader(void) {
#ifdef _WIN32
    HMODULE m = LoadLibraryA("openxr_loader.dll");
    if (!m) {                                                    /* 126 = the file or one of the DLLs it needs was not found, 193 = wrong architecture */
        fprintf(stderr, "pbvr: LoadLibrary(openxr_loader.dll) failed, Windows error %lu%s\n", GetLastError(),
                GetLastError() == 126 ? " (the DLL, or a DLL it depends on, is missing; a MinGW-built loader needs libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll beside it)" :
                GetLastError() == 193 ? " (the DLL is not a 64-bit x64 build)" : "");
        return NULL;
    }
    PFN_xrGetInstanceProcAddr f = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (!f) fprintf(stderr, "pbvr: openxr_loader.dll has no xrGetInstanceProcAddr (is it really the OpenXR loader?)\n");
    return f;
#else
    void *m = dlopen("libopenxr_loader.so.1", RTLD_NOW); if (!m) m = dlopen("libopenxr_loader.so", RTLD_NOW); if (!m) return NULL;
    return (PFN_xrGetInstanceProcAddr)dlsym(m, "xrGetInstanceProcAddr");
#endif
}

/* ------------------------------------------------------------------ GL framebuffer functions (not part of glscene's list) */
static PFNGLGENFRAMEBUFFERSPROC pGenFramebuffers; static PFNGLBINDFRAMEBUFFERPROC pBindFramebuffer; static PFNGLFRAMEBUFFERTEXTURE2DPROC pFramebufferTexture2D;
static PFNGLGENRENDERBUFFERSPROC pGenRenderbuffers; static PFNGLBINDRENDERBUFFERPROC pBindRenderbuffer; static PFNGLRENDERBUFFERSTORAGEPROC pRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC pFramebufferRenderbuffer; static PFNGLBLITFRAMEBUFFERPROC pBlitFramebuffer; static PFNGLCHECKFRAMEBUFFERSTATUSPROC pCheckFramebufferStatus;
static int load_fbo(void) {
#define L(v, T, n) v = (T)SDL_GL_GetProcAddress(n); if (!v) { fprintf(stderr, "pbvr: missing GL function %s\n", n); return -1; }
    L(pGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC, "glGenFramebuffers") L(pBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC, "glBindFramebuffer")
    L(pFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC, "glFramebufferTexture2D") L(pGenRenderbuffers, PFNGLGENRENDERBUFFERSPROC, "glGenRenderbuffers")
    L(pBindRenderbuffer, PFNGLBINDRENDERBUFFERPROC, "glBindRenderbuffer") L(pRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC, "glRenderbufferStorage")
    L(pFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC, "glFramebufferRenderbuffer") L(pBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC, "glBlitFramebuffer")
    L(pCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC, "glCheckFramebufferStatus")
#undef L
    return 0;
}

/* ------------------------------------------------------------------ small matrix helpers (column-major, double) */
typedef struct { double m[16]; } M4;
static M4 m4_id(void) { M4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }; return r; }
static M4 m4_mul(M4 a, M4 b) { M4 r; for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; } return r; }
static M4 m4_from_pose(XrPosef p) {                                         /* rigid transform, quaternion -> rotation */
    double x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w; M4 r = m4_id();
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w); r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w); r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w); r.m[9] = 2 * (y * z - x * w); r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = p.position.x; r.m[13] = p.position.y; r.m[14] = p.position.z; return r;
}
static M4 m4_rigid_inverse(M4 a) {
    M4 r = m4_id();
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[j * 4 + i] = a.m[i * 4 + j];            /* transpose of the rotation */
    for (int i = 0; i < 3; i++) r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    return r;
}
static M4 m4_yaw_pos(double yaw, const double p[3]) { M4 r = m4_id(); double c = cos(yaw), s = sin(yaw); r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; r.m[12] = p[0]; r.m[13] = p[1]; r.m[14] = p[2]; return r; }
static M4 m4_trans(double x, double y, double z) { M4 r = m4_id(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 m4_rotx(double a) { M4 r = m4_id(); double c = cos(a), s = sin(a); r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; return r; }
static M4 m4_scale_z(double zs) { M4 r = m4_id(); r.m[10] = zs; return r; }
static M4 m4_model(double zs, double pitch) { return m4_mul(m4_rotx(-pitch), m4_scale_z(zs)); }               /* arcade-camera frame -> world: depth scale, then tilt back by the camera's pitch */
static M4 m4_model_inv(double zs, double pitch) { return m4_mul(m4_scale_z(1.0 / zs), m4_rotx(pitch)); }
static M4 m4_proj(XrFovf f, double n, double fa) {
    double l = tan(f.angleLeft), r = tan(f.angleRight), u = tan(f.angleUp), d = tan(f.angleDown); M4 p; memset(&p, 0, sizeof p);
    p.m[0] = 2 / (r - l); p.m[5] = 2 / (u - d); p.m[8] = (r + l) / (r - l); p.m[9] = (u + d) / (u - d);
    p.m[10] = -(fa + n) / (fa - n); p.m[11] = -1; p.m[14] = -2 * fa * n / (fa - n); return p;
}
static void m4_point(M4 a, const double v[3], double out[3]) { for (int i = 0; i < 3; i++) out[i] = a.m[i] * v[0] + a.m[4 + i] * v[1] + a.m[8 + i] * v[2] + a.m[12 + i]; }
static void m4_dir(M4 a, const double v[3], double out[3]) { for (int i = 0; i < 3; i++) out[i] = a.m[i] * v[0] + a.m[4 + i] * v[1] + a.m[8 + i] * v[2]; }

typedef struct { int scene; Nb1ScaleSpec s; } OvEntry;           /* scene = nearest captured scene id, -1 when unknown */
typedef struct { int scene, has[3]; double pitch, ground, ceil; } SurAdj;                   /* per-scene adjustments of the world: tilt, floor, ceiling */
typedef struct { int kind, id, pri, has_key, key; } SelItem;     /* what the pointer picked, kept so the scope can be switched */

/* ------------------------------------------------------------------ options */
typedef struct {
    char roms[512], ipc[512], table[512], tables[512];
    double vscale, sdiag, sdist, gun_pitch, aim_yaw, aim_pitch, jump_dist; int no_labels, auto_on; char bot_rules[512], bot_stats[512]; int no_vr, gaze, no_surround, scene_start, live, no_brackets, hud_flat; double hud_below; char edits_out[512], scenes[512];
} Opts;

/* ------------------------------------------------------------------ scene worker: reads states and builds scenes off the render thread */
typedef struct {
    Opts *o; Nb1Roms *roms; Nb1TableSet *tset; Nb1Table *tab, *deftab;
    SDL_mutex *mu; GlScene back; int ready; unsigned back_frame; const char *back_table; int back_scene; double back_sim;
    volatile int quit, paused, rebuild; volatile double zs; Source src;
    OvEntry *ov; int nov, capov; SurAdj *adj; int nadj, capadj; volatile double hud_dy;                       /* the editor's edits, guarded by mu */
    Nb1Bot *bot; volatile int auto_on; volatile double bot_sx, bot_sy; volatile int bot_trig, bot_coin, bot_start, bot_valid; char bot_status[200];
    int prev_scroll[4][2], moving[4]; SceneEnt *scenes; int nscenes, live; const char *scenes_dir; volatile int goto_idx; Nb1Selector *selr; const Nb1Table *last_use; Nb1State *last; unsigned last_frame; const char *last_name; int last_scene; double last_sim;   /* the state the current scene came from, so an edit can rebuild it */
} Worker;

static int worker_main(void *arg) {
    Worker *w = arg;
    while (!w->quit) {
        SDL_LockMutex(w->mu); int busy = w->ready; SDL_UnlockMutex(w->mu);
        if (busy) { SDL_Delay(2); continue; }
        unsigned fr = 0; Nb1State *st = NULL; int frozen = w->scenes && !w->live, forced_scene = -1;
        if (frozen) { int gi = w->goto_idx; if (gi >= 0 && gi < w->nscenes) { w->goto_idx = -1; st = scene_state_load(w->scenes_dir, &w->scenes[gi]); forced_scene = w->scenes[gi].id;
                                         if (w->selr) { nb1_selector_free(w->selr); w->selr = nb1_selector_new(w->tset, 1); } } }       /* a new snapshot: choose its table at once */
        else if (!w->paused) st = poll_state(&w->src, &fr);
        if (st) {
            for (int L = 0; L < 4; L++) {                                  /* layers whose scroll registers keep changing are scrolling: their world continues the game's own map */
                int sx = st->ctl[4 * L + 1] & 0x1ff, sy = st->ctl[4 * L + 3] & 0x1ff;
                if (sx != w->prev_scroll[L][0] || sy != w->prev_scroll[L][1]) w->moving[L] = 24; else if (w->moving[L] > 0) w->moving[L]--;
                w->prev_scroll[L][0] = sx; w->prev_scroll[L][1] = sy; w->back.scroll_hint[L] = w->moving[L] > 0;
            }
            free(w->last); w->last = st; w->last_frame = fr; w->last_name = NULL; w->last_scene = -1; w->last_sim = 0;
            if (forced_scene >= 0) w->last_scene = forced_scene; else if (w->tset) w->last_scene = nb1_tableset_nearest_scene(w->tset, st, &w->last_sim);
        } else if (w->rebuild && w->last) { /* an edit: build the same state again */ }
        else { SDL_Delay(2); continue; }
        w->rebuild = 0;
        const Nb1Table *use = w->deftab; const char *name = NULL;
        if (w->tab) use = w->tab;
        else if (w->selr) {
            if (st) { const Nb1Table *sel = nb1_selector_update(w->selr, w->last, w->last, &name); w->last_use = sel; w->last_name = name; }     /* only a NEW state moves the selector */
            if (w->last_use) use = w->last_use;
            name = w->last_name;
        }
        w->last_name = name;
        if (w->bot && st && !frozen) {                                       /* the auto-player sees every new state (the arcade's own sprite list) */
            Nb1GameInfo gi; memset(&gi, 0, sizeof gi); char gp[1100]; size_t gn; snprintf(gp, sizeof gp, "%s/game.txt", w->src.ipc); uint8_t *gb = slurp(gp, &gn);
            if (gb) { gb = realloc(gb, gn + 1); gb[gn] = 0; unsigned gf; int l, sc, m, sg, d; if (sscanf((char *)gb, "%u %d %d %d %d %d", &gf, &l, &sc, &m, &sg, &d) == 6) { gi.valid = 1; gi.lives = l; gi.score = sc; gi.mode = m; gi.stage = sg; gi.diff = d; } free(gb); }
            nb1_bot_set_scene(w->bot, name);
            double bsx, bsy; int bt, bc, bs; nb1_bot_step(w->bot, st, &gi, fr, &bsx, &bsy, &bt, &bc, &bs);
            if (w->auto_on) { w->bot_sx = bsx; w->bot_sy = bsy; w->bot_trig = bt; w->bot_coin = bc; w->bot_start = bs; w->bot_valid = 1; }
            SDL_LockMutex(w->mu); snprintf(w->bot_status, sizeof w->bot_status, "%s", nb1_bot_status(w->bot)); SDL_UnlockMutex(w->mu);
        }
        Nb1ScaleSpec specs[64]; int ns = 0;
        SDL_LockMutex(w->mu);
        for (int i = 0; i < w->nov && ns < 64; i++) if (w->ov[i].scene == w->last_scene && w->ov[i].s.k != 1.0) specs[ns++] = w->ov[i].s;
        SDL_UnlockMutex(w->mu);
        Nb1Table *scaled = ns ? nb1_table_with_scales(use, specs, ns) : NULL;
        w->back.adj_has[0] = w->back.adj_has[1] = w->back.adj_has[2] = 0; w->back.hud_dy = w->hud_dy;
        SDL_LockMutex(w->mu); for (int i = 0; i < w->nadj; i++) if (w->adj[i].scene == w->last_scene) { for (int k = 0; k < 3; k++) w->back.adj_has[k] = w->adj[i].has[k]; w->back.adj_pitch = w->adj[i].pitch; w->back.adj_ground = w->adj[i].ground; w->back.adj_ceil = w->adj[i].ceil; } SDL_UnlockMutex(w->mu);
        w->back.zs = w->zs; w->back.no_rulers = 1; w->back.surround = !w->o->no_surround;
        gl_scene_cpu(&w->back, w->last, w->roms, scaled ? scaled : use); w->back.scene.aim_skip_hud = 1;
        nb1_table_free(scaled);
        SDL_LockMutex(w->mu); w->ready = 1; w->back_frame = w->last_frame; w->back_table = name; w->back_scene = w->last_scene; w->back_sim = w->last_sim; SDL_UnlockMutex(w->mu);
    }
    return 0;
}

/* ------------------------------------------------------------------ OpenXR session */
typedef struct { XrSwapchain sc; int w, h; uint32_t n; XrSwapchainImageOpenGLKHR *img; GLuint fbo, depth; } Eye;
typedef struct {
    XrInstance inst; XrSystemId sys; XrSession sess; XrSpace local, aim_space[2]; int running, exit_requested, focused;
    XrActionSet set; XrAction a_aim, a_trigger, a_coin, a_start, a_pause, a_recenter, a_haptic, a_squeeze, a_stick; XrPath hand[2];
    Eye eye[2]; XrView views[2]; XrViewConfigurationView vcv[2]; XrTime pred;
} Xr;

#define CHECK(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { char b_[XR_MAX_RESULT_STRING_SIZE]; xrResultToString(xr->inst, r_, b_); fprintf(stderr, "pbvr: %s failed: %s\n", #call, b_); return -1; } } while (0)

static XrPath P(Xr *xr, const char *s) { XrPath p = 0; xrStringToPath(xr->inst, s, &p); return p; }

static int make_action(Xr *xr, XrAction *out, const char *name, XrActionType type, int both) {
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO }; ai.actionType = type; snprintf(ai.actionName, sizeof ai.actionName, "%s", name); snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", name);
    if (both) { ai.countSubactionPaths = 2; ai.subactionPaths = xr->hand; }
    CHECK(xrCreateAction(xr->set, &ai, out)); return 0;
}
static int suggest(Xr *xr, const char *profile, const XrActionSuggestedBinding *b, int n) {
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING }; s.interactionProfile = P(xr, profile); s.suggestedBindings = b; s.countSuggestedBindings = (uint32_t)n;
    XrResult r = xrSuggestInteractionProfileBindings(xr->inst, &s); if (XR_FAILED(r)) fprintf(stderr, "pbvr: bindings for %s refused (%d)\n", profile, (int)r); return 0;
}
static int actions_init(Xr *xr) {
    xr->hand[0] = P(xr, "/user/hand/left"); xr->hand[1] = P(xr, "/user/hand/right");
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO }; snprintf(si.actionSetName, sizeof si.actionSetName, "game"); snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Point Blank");
    CHECK(xrCreateActionSet(xr->inst, &si, &xr->set));
    if (make_action(xr, &xr->a_aim, "aim", XR_ACTION_TYPE_POSE_INPUT, 1) || make_action(xr, &xr->a_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
        make_action(xr, &xr->a_coin, "coin", XR_ACTION_TYPE_BOOLEAN_INPUT, 1) || make_action(xr, &xr->a_start, "start", XR_ACTION_TYPE_BOOLEAN_INPUT, 1) ||
        make_action(xr, &xr->a_pause, "pause", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) || make_action(xr, &xr->a_recenter, "recenter", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        make_action(xr, &xr->a_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1) ||
        make_action(xr, &xr->a_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, 1) || make_action(xr, &xr->a_stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 1)) return -1;
    XrActionSuggestedBinding b[32]; int n = 0;
#define B(act, path) b[n].action = act; b[n++].binding = P(xr, path);
    B(xr->a_aim, "/user/hand/left/input/aim/pose") B(xr->a_aim, "/user/hand/right/input/aim/pose")
    B(xr->a_trigger, "/user/hand/left/input/trigger/value") B(xr->a_trigger, "/user/hand/right/input/trigger/value")
    B(xr->a_coin, "/user/hand/left/input/x/click") B(xr->a_coin, "/user/hand/right/input/a/click")
    B(xr->a_start, "/user/hand/left/input/y/click") B(xr->a_start, "/user/hand/right/input/b/click")
    B(xr->a_pause, "/user/hand/left/input/thumbstick/click") B(xr->a_recenter, "/user/hand/right/input/thumbstick/click")
    B(xr->a_haptic, "/user/hand/left/output/haptic") B(xr->a_haptic, "/user/hand/right/output/haptic")
    B(xr->a_squeeze, "/user/hand/left/input/squeeze/value") B(xr->a_squeeze, "/user/hand/right/input/squeeze/value")
    B(xr->a_stick, "/user/hand/left/input/thumbstick") B(xr->a_stick, "/user/hand/right/input/thumbstick")
    suggest(xr, "/interaction_profiles/oculus/touch_controller", b, n);
    n = 0;                                                                   /* Valve Index: A/B exist on both hands */
    B(xr->a_aim, "/user/hand/left/input/aim/pose") B(xr->a_aim, "/user/hand/right/input/aim/pose")
    B(xr->a_trigger, "/user/hand/left/input/trigger/value") B(xr->a_trigger, "/user/hand/right/input/trigger/value")
    B(xr->a_coin, "/user/hand/right/input/a/click") B(xr->a_start, "/user/hand/right/input/b/click")
    B(xr->a_pause, "/user/hand/left/input/thumbstick/click") B(xr->a_recenter, "/user/hand/right/input/thumbstick/click")
    B(xr->a_haptic, "/user/hand/left/output/haptic") B(xr->a_haptic, "/user/hand/right/output/haptic")
    B(xr->a_squeeze, "/user/hand/left/input/squeeze/value") B(xr->a_squeeze, "/user/hand/right/input/squeeze/value")
    B(xr->a_stick, "/user/hand/left/input/thumbstick") B(xr->a_stick, "/user/hand/right/input/thumbstick")
    suggest(xr, "/interaction_profiles/valve/index_controller", b, n);
    n = 0;                                                                   /* KHR simple controller: select = fire, menu = start */
    B(xr->a_aim, "/user/hand/left/input/aim/pose") B(xr->a_aim, "/user/hand/right/input/aim/pose")
    B(xr->a_trigger, "/user/hand/left/input/select/click") B(xr->a_trigger, "/user/hand/right/input/select/click")
    B(xr->a_start, "/user/hand/left/input/menu/click") B(xr->a_start, "/user/hand/right/input/menu/click")
    B(xr->a_haptic, "/user/hand/left/output/haptic") B(xr->a_haptic, "/user/hand/right/output/haptic")
    suggest(xr, "/interaction_profiles/khr/simple_controller", b, n);
#undef B
    return 0;
}

static int xr_init(Xr *xr, SDL_Window *win, PFN_xrGetInstanceProcAddr gipa) {
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO }; const char *ext[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Point Blank VR"); ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "nb1"); ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);     /* ask for 1.0: newer headers' XR_CURRENT_API_VERSION (1.1) is refused by runtimes that only support 1.0 */
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = ext;
    PFN_xrCreateInstance create = NULL; if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create))) { fprintf(stderr, "pbvr: loader has no xrCreateInstance\n"); return -1; }
    XrResult r = create(&ci, &xr->inst);
    if (XR_FAILED(r)) { fprintf(stderr, "pbvr: xrCreateInstance failed (%d%s). Is an OpenXR runtime with OpenGL support active (SteamVR, Meta Quest Link / Oculus)?\n", (int)r, r == XR_ERROR_API_VERSION_UNSUPPORTED ? ", API version unsupported" : r == XR_ERROR_RUNTIME_UNAVAILABLE ? ", no runtime" : ""); return -1; }
    if (load_openxr(xr->inst, gipa)) return -1;
    if (XR_FAILED(gipa(xr->inst, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pxrGetOpenGLGraphicsRequirementsKHR))) { fprintf(stderr, "pbvr: no XR_KHR_opengl_enable\n"); return -1; }
    XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO }; gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    CHECK(xrGetSystem(xr->inst, &gi, &xr->sys));
    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR }; pxrGetOpenGLGraphicsRequirementsKHR(xr->inst, xr->sys, &req);
    fprintf(stderr, "pbvr: runtime wants OpenGL >= %u.%u\n", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported));
#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR bind = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR }; bind.hDC = wglGetCurrentDC(); bind.hGLRC = wglGetCurrentContext();
#else
    XrGraphicsBindingOpenGLXlibKHR bind = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR }; bind.xDisplay = XOpenDisplay(NULL); bind.glxDrawable = glXGetCurrentDrawable();
    bind.glxContext = glXGetCurrentContext(); { int n = 0; GLXFBConfig *fb = glXGetFBConfigs(bind.xDisplay, DefaultScreen(bind.xDisplay), &n); (void)fb; }
    { int id = 0; glXQueryContext(bind.xDisplay, bind.glxContext, GLX_FBCONFIG_ID, &id); int n = 0; int attr[] = { GLX_FBCONFIG_ID, id, 0 };
      GLXFBConfig *fb = glXChooseFBConfig(bind.xDisplay, DefaultScreen(bind.xDisplay), attr, &n); if (fb && n) { bind.glxFBConfig = fb[0]; XVisualInfo *vi = glXGetVisualFromFBConfig(bind.xDisplay, fb[0]); if (vi) bind.visualid = (uint32_t)vi->visualid; } }
#endif
    (void)win;
    XrSessionCreateInfo si = { XR_TYPE_SESSION_CREATE_INFO }; si.next = &bind; si.systemId = xr->sys; CHECK(xrCreateSession(xr->inst, &si, &xr->sess));
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1;
    CHECK(xrCreateReferenceSpace(xr->sess, &rs, &xr->local));
    uint32_t nv = 0; CHECK(xrEnumerateViewConfigurationViews(xr->inst, xr->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, NULL));
    if (nv != 2) { fprintf(stderr, "pbvr: expected 2 views, got %u\n", nv); return -1; }
    for (int i = 0; i < 2; i++) xr->vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    CHECK(xrEnumerateViewConfigurationViews(xr->inst, xr->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, xr->vcv));
    uint32_t nf = 0; CHECK(xrEnumerateSwapchainFormats(xr->sess, 0, &nf, NULL)); int64_t *fm = malloc(nf * sizeof *fm); CHECK(xrEnumerateSwapchainFormats(xr->sess, nf, &nf, fm));
    int64_t fmt = 0; for (uint32_t i = 0; i < nf; i++) if (fm[i] == GL_RGBA8) fmt = fm[i];                     /* the scene's colours are display values: no sRGB conversion */
    if (!fmt) { fprintf(stderr, "pbvr: runtime offers no plain RGBA8 swapchain; using its first format\n"); fmt = fm[0]; }
    free(fm);
    for (int e = 0; e < 2; e++) {
        Eye *E = &xr->eye[e]; E->w = (int)xr->vcv[e].recommendedImageRectWidth; E->h = (int)xr->vcv[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO }; sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT; sc.format = fmt;
        sc.sampleCount = 1; sc.width = (uint32_t)E->w; sc.height = (uint32_t)E->h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        CHECK(xrCreateSwapchain(xr->sess, &sc, &E->sc)); CHECK(xrEnumerateSwapchainImages(E->sc, 0, &E->n, NULL));
        E->img = calloc(E->n, sizeof *E->img); for (uint32_t i = 0; i < E->n; i++) E->img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        CHECK(xrEnumerateSwapchainImages(E->sc, E->n, &E->n, (XrSwapchainImageBaseHeader *)E->img));
        pGenFramebuffers(1, &E->fbo); pGenRenderbuffers(1, &E->depth); pBindRenderbuffer(GL_RENDERBUFFER, E->depth); pRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, E->w, E->h);
        fprintf(stderr, "pbvr: eye %d %dx%d, %u images\n", e, E->w, E->h, E->n);
    }
    if (actions_init(xr)) return -1;
    XrActionSpaceCreateInfo as = { XR_TYPE_ACTION_SPACE_CREATE_INFO }; as.action = xr->a_aim; as.poseInActionSpace.orientation.w = 1;
    for (int h = 0; h < 2; h++) { as.subactionPath = xr->hand[h]; CHECK(xrCreateActionSpace(xr->sess, &as, &xr->aim_space[h])); }
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO }; at.countActionSets = 1; at.actionSets = &xr->set; CHECK(xrAttachSessionActionSets(xr->sess, &at));
    return 0;
}

static void xr_events(Xr *xr) {
    for (;;) {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER }; if (xrPollEvent(xr->inst, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged *)&ev;
            fprintf(stderr, "pbvr: session state %d\n", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) { XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; xrBeginSession(xr->sess, &bi); xr->running = 1; }
            else if (s->state == XR_SESSION_STATE_STOPPING) { xrEndSession(xr->sess); xr->running = 0; }
            else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) xr->exit_requested = 1;
            xr->focused = s->state == XR_SESSION_STATE_FOCUSED;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) xr->exit_requested = 1;
    }
}

/* ------------------------------------------------------------------ drawing */
typedef struct { GLuint prog; GLint uMVP, uTex, uMode, uBox, uFadeC, uFadeCol; GLuint vao, vbo, line_vao, line_vbo; } Gfx;

static void draw_scene(const Gfx *g, const GlScene *gs, const M4 *mvp) {
    float m[16]; for (int i = 0; i < 16; i++) m[i] = (float)mvp->m[i];
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glUseProgram(g->prog); glUniformMatrix4fv(g->uMVP, 1, GL_FALSE, m); glUniform1i(g->uTex, 0); glBindVertexArray(g->vao);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_SRC_COLOR); glDepthFunc(GL_LESS); glDepthMask(GL_FALSE); }
        for (int oi = 0; oi < gs->ndraws; oi++) {
            int i = gs->order[oi]; const Draw *d = &gs->draws[i];
            if (!d->count || (pass == 1 && !d->shadow_tex)) continue;
            glBindTexture(GL_TEXTURE_2D, pass ? d->shadow_tex : d->tex);
            glUniform1i(g->uMode, pass ? 2 : d->mode); glUniform4f(g->uBox, (float)d->box[0], (float)d->box[1], (float)d->box[2], (float)d->box[3]);
            if (d->mode == 5) { glUniform4f(g->uBox, d->fr[0], d->fr[1], d->fkind, 0); glUniform4f(g->uFadeC, d->fc[0], d->fc[1], d->fc[2], 0); glUniform4f(g->uFadeCol, d->fcol[0], d->fcol[1], d->fcol[2], 0); }
            glDrawArrays(GL_TRIANGLES, d->first, d->count);
        }
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL);
}
static void draw_lines(const Gfx *g, const M4 *mvp, const float *xyz, int nverts, float r, float gr, float b) {
    float m[16]; for (int i = 0; i < 16; i++) m[i] = (float)mvp->m[i];
    Vtx v[96]; for (int i = 0; i < nverts && i < 96; i++) v[i] = (Vtx){ xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2], 0, 0 };
    glDisable(GL_DEPTH_TEST); glUseProgram(g->prog); glUniformMatrix4fv(g->uMVP, 1, GL_FALSE, m); glUniform1i(g->uMode, 3); glUniform4f(g->uBox, r, gr, b, 1);
    glBindVertexArray(g->line_vao); glBindBuffer(GL_ARRAY_BUFFER, g->line_vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(Vtx) * nverts, v, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, nverts); glBindVertexArray(g->vao); glBindBuffer(GL_ARRAY_BUFFER, g->vbo); glEnable(GL_DEPTH_TEST);
}

#include "editor.h"

/* Appends one line to the edits file: which scene and layers were changed and by how much, plus a "scales" snippet for the scene's table.
 * Returns the number of edits written, or -1. The caller holds W->mu. */
static int save_edits(const char *path, const Worker *W, const GlScene *g, int scene, double sim, const char *table, unsigned frame, const Opts *o, double zs) {
    FILE *f = fopen(path, "ab"); if (!f) return -1;
    char ts[40]; time_t now = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    fprintf(f, "{\"saved\": \"%s\", \"scene\": {\"table\": \"%s\", \"nearest_scene\": %d, \"similarity\": %.3f, \"game_frame\": %u}, ", ts, table ? table : "default", scene, sim, frame);
    fprintf(f, "\"viewport\": {\"screen_diag_in\": %.1f, \"screen_dist_m\": %.3f, \"viewport_scale\": %.3f, \"depth_scale\": %.4f}, \"scales\": [", o->sdiag, o->sdist, o->vscale, zs);
    int n = 0; char sj[160], sd[80];
    for (int i = 0; i < W->nov; i++) {
        const OvEntry *e = &W->ov[i]; if (e->scene != scene) continue;
        double zm = 0; int items = group_z(g, &e->s, &zm); spec_json(&e->s, sj, sizeof sj); spec_desc(&e->s, sd, sizeof sd);
        fprintf(f, "%s{\"layer\": \"%s\", \"match\": %s, \"scale\": %.4f, \"items\": %d, \"z_authored_before\": %.3f, \"z_authored_after\": %.3f, \"z_physical_m\": %.3f}", n ? ", " : "", sd, sj, e->s.k, items, zm / e->s.k, zm, zm * zs);
        n++;
    }
    fprintf(f, "], \"table_snippet\": {\"scales\": [");
    int m = 0;
    for (int i = 0; i < W->nov; i++) { const OvEntry *e = &W->ov[i]; if (e->scene != scene) continue; spec_json(&e->s, sj, sizeof sj); fprintf(f, "%s{\"match\": %s, \"scale\": %.4f}", m++ ? ", " : "", sj, e->s.k); }
    fprintf(f, "]}, \"world\": {\"pitch_deg\": %.2f, \"ground_y_m\": %s, \"ceiling_y_m\": %s, \"hud_below_px\": %.0f, \"pitch_adjusted\": %d, \"ground_adjusted\": %d, \"ceiling_adjusted\": %d}", g->scene.pitch_deg,
            isnan(g->scene.ground_y) ? "null" : (snprintf(sj, sizeof sj, "%.3f", g->scene.ground_y), sj), isnan(g->scene.ceil_y) ? "null" : (snprintf(sd, sizeof sd, "%.3f", g->scene.ceil_y), sd), o->hud_flat ? 0.0 : o->hud_below, g->scene.adj_has[0], g->scene.adj_has[1], g->scene.adj_has[2]);
    fprintf(f, "}\n"); fclose(f); return n;
}

typedef struct { char s[48]; GLuint tex; int w, h; } LabelTex;                               /* small text labels, made once per distinct text */
typedef struct { LabelTex e[96]; int n; } LabelCache;
static LabelTex *label_get(LabelCache *c, const char *s) {
    for (int i = 0; i < c->n; i++) if (!strcmp(c->e[i].s, s)) return &c->e[i];
    if (c->n >= 96) return NULL;
    LabelTex *l = &c->e[c->n++]; snprintf(l->s, sizeof l->s, "%s", s); l->tex = text_tex(s, &l->w, &l->h); return l;
}

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {            /* a silent exit in PowerShell is hard to debug: say what happened */
    fprintf(stderr, "\npbvr: CRASH, exception 0x%08lx%s at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionCode == 0xC00000FD ? " (stack overflow)" : e->ExceptionRecord->ExceptionCode == 0xC0000005 ? " (access violation)" : "", e->ExceptionRecord->ExceptionAddress);
    if (e->ExceptionRecord->ExceptionCode == 0xC0000005 && e->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "pbvr: %s address %p\n", e->ExceptionRecord->ExceptionInformation[0] ? "wrote" : "read", (void *)e->ExceptionRecord->ExceptionInformation[1]);
    fflush(stderr); return EXCEPTION_EXECUTE_HANDLER;
}
#endif
static char adjf[32], adjc[32];
int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter); setvbuf(stderr, NULL, _IONBF, 0);
#endif
    Opts o; memset(&o, 0, sizeof o); o.vscale = 1; o.sdiag = 29; o.sdist = 0.9; o.jump_dist = 1.0; o.hud_below = 72; snprintf(o.edits_out, sizeof o.edits_out, "pbvr_layer_edits.jsonl");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--roms") && v) { snprintf(o.roms, sizeof o.roms, "%s", v); i++; }
        else if (!strcmp(a, "--ipc") && v) { snprintf(o.ipc, sizeof o.ipc, "%s", v); i++; }
        else if (!strcmp(a, "--table") && v) { snprintf(o.table, sizeof o.table, "%s", v); i++; }
        else if (!strcmp(a, "--tables") && v) { snprintf(o.tables, sizeof o.tables, "%s", v); i++; }
        else if (!strcmp(a, "--screen-diag") && v) { o.sdiag = atof(v); i++; }
        else if (!strcmp(a, "--screen-dist") && v) { o.sdist = atof(v); i++; }
        else if (!strcmp(a, "--viewport-scale") && v) { o.vscale = atof(v); i++; }
        else if (!strcmp(a, "--gun-pitch") && v) { o.gun_pitch = atof(v); i++; }
        else if (!strcmp(a, "--no-vr")) o.no_vr = 1;
        else if (!strcmp(a, "--no-surround")) o.no_surround = 1;
        else if (!strcmp(a, "--auto")) o.auto_on = 1;                                                           /* the auto-player starts on (T key, or squeeze both grips for a second, toggles it) */
        else if (!strcmp(a, "--bot-rules") && v) { snprintf(o.bot_rules, sizeof o.bot_rules, "%s", v); i++; }   /* default: bot_rules.json beside the tables folder */
        else if (!strcmp(a, "--bot-stats") && v) { snprintf(o.bot_stats, sizeof o.bot_stats, "%s", v); i++; }   /* what the player learned, written when it exits */
        else if (!strcmp(a, "--no-labels")) o.no_labels = 1;                                                  /* no scene name under the picture, no names on the HUD sprites */
        else if (!strcmp(a, "--no-brackets")) o.no_brackets = 1;                                              /* no white corner marks round the play window */
        else if (!strcmp(a, "--hud-overlay")) o.hud_flat = 1;                                                  /* keep the HUD on the picture (default: moved below it) */
        else if (!strcmp(a, "--hud-below") && v) { o.hud_below = atof(v); i++; }                                /* arcade pixels the HUD is moved down (default 72) */
        else if (!strcmp(a, "--scenes") && v) { snprintf(o.scenes, sizeof o.scenes, "%s", v); i++; }             /* the frozen scene snapshots (research/data/scenes) */
        else if (!strcmp(a, "--scene") && v) { o.scene_start = atoi(v); i++; }
        else if (!strcmp(a, "--live")) o.live = 1;                                                              /* with --scenes: jump MAME to each scene's save state instead of showing the frozen snapshot */
        else if (!strcmp(a, "--edits-out") && v) { snprintf(o.edits_out, sizeof o.edits_out, "%s", v); i++; }
        else if (!strcmp(a, "--jump-dist") && v) { o.jump_dist = atof(v); i++; }
        else if (!strcmp(a, "--gaze-aim")) o.gaze = 1;                                   /* aim with the head instead of a controller (testing, or no controllers) */
        else if (!strcmp(a, "--aim-offset") && v) { sscanf(v, "%lf,%lf", &o.aim_yaw, &o.aim_pitch); i++; }   /* yaw,pitch degrees added to the aim ray (testing) */
        else { fprintf(stderr, "usage: pbvr --roms DIR --ipc DIR [--tables scene_tables.json | --table T.json] [--screen-diag IN] [--screen-dist M] [--viewport-scale K] [--gun-pitch DEG] [--no-vr]\n"); return 2; }
    }
    if (!o.roms[0] || (!o.ipc[0] && !(o.scenes[0] && !o.live))) { fprintf(stderr, "pbvr: --roms and --ipc are required (--ipc can be left out with --scenes DIR and no --live)\n"); return 2; }
    Nb1Roms roms; if (nb1_roms_load(&roms, o.roms)) { fprintf(stderr, "pbvr: cannot load ROMs from %s\n", o.roms); return 1; }

    PFN_xrGetInstanceProcAddr gipa = o.no_vr ? NULL : open_loader();
    if (!o.no_vr && !gipa) { fprintf(stderr, "pbvr: cannot load the OpenXR loader (openxr_loader.dll / libopenxr_loader.so.1). Put it next to pbvr.exe, or install your headset software's OpenXR runtime.\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { fprintf(stderr, "pbvr: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("Point Blank VR (mirror of the left eye)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 360, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "pbvr: window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win); if (!ctx) { fprintf(stderr, "pbvr: OpenGL 3.3 context: %s\n", SDL_GetError()); return 1; }
    if (load_gl() || load_fbo()) return 1;
    SDL_GL_SetSwapInterval(0);                                               /* the headset paces the frames, not the mirror */

    Gfx gfx; memset(&gfx, 0, sizeof gfx); gfx.prog = program(VS, FS);
    gfx.uMVP = glGetUniformLocation(gfx.prog, "uMVP"); gfx.uTex = glGetUniformLocation(gfx.prog, "uTex"); gfx.uMode = glGetUniformLocation(gfx.prog, "uMode"); gfx.uBox = glGetUniformLocation(gfx.prog, "uBox"); gfx.uFadeC = glGetUniformLocation(gfx.prog, "uFadeC"); gfx.uFadeCol = glGetUniformLocation(gfx.prog, "uFadeCol");
    glGenVertexArrays(1, &gfx.vao); glBindVertexArray(gfx.vao); glGenBuffers(1, &gfx.vbo); glBindBuffer(GL_ARRAY_BUFFER, gfx.vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(3 * sizeof(float)));
    glGenVertexArrays(1, &gfx.line_vao); glBindVertexArray(gfx.line_vao); glGenBuffers(1, &gfx.line_vbo); glBindBuffer(GL_ARRAY_BUFFER, gfx.line_vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(3 * sizeof(float)));
    glBindVertexArray(gfx.vao); glBindBuffer(GL_ARRAY_BUFFER, gfx.vbo);

    Xr xr; memset(&xr, 0, sizeof xr);
    if (!o.no_vr && xr_init(&xr, win, gipa)) return 1;

    /* scene worker */
    Worker W; memset(&W, 0, sizeof W); W.o = &o; W.roms = &roms; W.deftab = nb1_table_default(); W.mu = SDL_CreateMutex();
    if (o.table[0]) { W.tab = nb1_table_load(o.table); if (!W.tab) fprintf(stderr, "pbvr: cannot load table %s\n", o.table); }
    else if (o.tables[0]) { W.tset = nb1_tableset_load(o.tables); if (!W.tset) fprintf(stderr, "pbvr: cannot load tables %s\n", o.tables); else { nb1_tableset_set_roms(W.tset, &roms); W.selr = nb1_selector_new(W.tset, 3); } }
    snprintf(W.src.ipc, sizeof W.src.ipc, "%s", o.ipc);
    {                                                                                                              /* the auto-player (always built; it only acts while switched on) */
        char rp[1100]; if (o.bot_rules[0]) snprintf(rp, sizeof rp, "%s", o.bot_rules); else { char dir[1000]; snprintf(dir, sizeof dir, "%s", o.tables[0] ? o.tables : "./x"); char *s1 = strrchr(dir, '/'), *s2 = strrchr(dir, '\\'), *sl = s1 > s2 ? s1 : s2; if (sl) *sl = 0; else strcpy(dir, "."); snprintf(rp, sizeof rp, "%s/../bot_rules.json", dir); }
        W.bot = nb1_bot_new(rp); W.auto_on = o.auto_on;
    }
    double vw_m = o.sdiag * 0.0254 * 0.8 * o.vscale, zs = (NB1_W * o.sdist / vw_m) / NB1_FOCAL; W.zs = zs;
    SceneEnt *scn = NULL; int nscn = 0, scn_idx = 0; char load_name[64] = ""; int load_seq = 0; char scene_desc[260] = "";
    if (o.scenes[0]) {
        scn = scene_index_load(o.scenes, &nscn); if (!scn || !nscn) { fprintf(stderr, "pbvr: no scenes.json in %s\n", o.scenes); return 1; }
        for (int i = 0; i < nscn; i++) if (scn[i].id == o.scene_start) scn_idx = i;
        W.scenes = scn; W.nscenes = nscn; W.scenes_dir = o.scenes; W.live = o.live; W.goto_idx = -1;
        if (W.tset) { if (W.selr) nb1_selector_free(W.selr); W.selr = nb1_selector_new(W.tset, o.live ? 3 : 1); }
        fprintf(stderr, "pbvr: %d scenes from %s (%s)\n", nscn, o.scenes, o.live ? "live: MAME jumps to each save state" : "frozen snapshots, no MAME needed");
    }
    SDL_Thread *thr = SDL_CreateThreadWithStackSize(worker_main, "scene", 64 << 20, &W);   /* the scene builder keeps big buffers on the stack; Windows threads default to 1 MB */

    if (scn) {
        snprintf(scene_desc, sizeof scene_desc, "SCENE %d  %s  (%d of %d)%s%s", scn[scn_idx].id, scn[scn_idx].family, scn_idx + 1, nscn, scn[scn_idx].label[0] ? "  " : "", scn[scn_idx].label);
        if (!o.live) W.goto_idx = scn_idx; else { snprintf(load_name, sizeof load_name, "%s@%d", scn[scn_idx].state, ++load_seq); }
    }
    GlScene front; memset(&front, 0, sizeof front); front.zs = zs; unsigned shown_frame = 0; const char *table_name = NULL;
    double origin[3] = { 0, 0, 0 }, yaw0 = 0; int need_recenter = 1;
    char aim_path[1100], aim_tmp[1100]; snprintf(aim_path, sizeof aim_path, "%s/aim.txt", o.ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s/aim.tmp", o.ipc);
    int seq = 0, trig = 0, coin = 0, start = 0, paused = (o.scenes[0] && !o.live) ? 1 : 0, kcoin = 0, kstart = 0, active_hand = 1, prev_trig[2] = { 0, 0 }, prev_pause = 0, prev_rec = 0;
    double sx = 144, sy = 112, last_aim_write = 0, fps_t = now_s(); int fps_n = 0; int running = 1;
    int shot_wait = 0;
    /* pause-mode editor state */
    Panel namep; memset(&namep, 0, sizeof namep); LabelCache lc; memset(&lc, 0, sizeof lc);
    Panel panel, side_l, side_r; memset(&panel, 0, sizeof panel); memset(&side_l, 0, sizeof side_l); memset(&side_r, 0, sizeof side_r);
    int has_sel = 0, scope = 0, cur_scene = -1; double cur_sim = 0; Nb1ScaleSpec sel; SelItem sel_item; memset(&sel, 0, sizeof sel); memset(&sel_item, 0, sizeof sel_item);
    double jump = 0, last_t = now_s(); int prev_sq[2] = { 0, 0 }, prev_ba = 0, prev_bb = 0, prev_bx = 0, prev_by = 0, prev_seltrig = 0;
    int inj_sel = 0, inj_sq = 0, inj_a = 0, inj_b = 0, inj_x = 0, inj_y = 0, kb_ls = 0, kb_rs = 0, af = 0, shot_req = 0, shot_n = 0; const int autotest = getenv("PBVR_AUTOTEST") != NULL || getenv("PBVR_TOUR") != NULL, tour = getenv("PBVR_TOUR") != NULL;   /* keyboard / scripted stand-ins for the controller buttons */
    int need_msg_adj = 0, adj_target = 0; static const char *ADJ_NAME[5] = { "viewport size", "screen tilt", "floor height", "ceiling height", "HUD below the picture" };
    int nav = 0, prev_flick = 0; float stick_x[2] = { 0, 0 };
    int in_a[2] = { 0, 0 }, in_b[2] = { 0, 0 }, in_sq[2] = { 0, 0 }; float stick[2] = { 0, 0 }; char msg[160] = ""; double msg_until = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { fprintf(stderr, "pbvr: window closed\n"); running = 0; }
            else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: fprintf(stderr, "pbvr: ESC pressed\n"); running = 0; break;
                case SDLK_c: kcoin = 1; break; case SDLK_v: kstart = 1; break;
                case SDLK_t: W.auto_on = !W.auto_on; snprintf(msg, sizeof msg, "auto-player %s", W.auto_on ? "ON" : "off"); msg_until = now_s() + 3; break;
                case SDLK_p: paused = !paused; break; case SDLK_r: need_recenter = 1; break;
                case SDLK_LEFTBRACKET: o.vscale /= 1.1; break; case SDLK_RIGHTBRACKET: o.vscale *= 1.1; break;
                case SDLK_RETURN: inj_sel = 1; break; case SDLK_g: inj_sq = 1; break; case SDLK_j: inj_a = 1; break; case SDLK_s: inj_b = 1; break; case SDLK_x: inj_x = 1; break; case SDLK_y: inj_y = 1; break;
                case SDLK_RIGHT: nav = 1; break; case SDLK_LEFT: nav = -1; break;
                case SDLK_UP: kb_ls = 1; break; case SDLK_DOWN: kb_ls = -1; break; case SDLK_PAGEUP: kb_rs = 1; break; case SDLK_PAGEDOWN: kb_rs = -1; break;
                default: break;
            } else if (ev.type == SDL_KEYUP) { if (ev.key.keysym.sym == SDLK_c) kcoin = 0; if (ev.key.keysym.sym == SDLK_v) kstart = 0; if (ev.key.keysym.sym == SDLK_UP || ev.key.keysym.sym == SDLK_DOWN) kb_ls = 0; if (ev.key.keysym.sym == SDLK_PAGEUP || ev.key.keysym.sym == SDLK_PAGEDOWN) kb_rs = 0; }
        }
        vw_m = o.sdiag * 0.0254 * 0.8 * o.vscale; zs = (NB1_W * o.sdist / vw_m) / NB1_FOCAL; W.zs = zs; W.paused = paused;
        { double hd = o.hud_flat ? 0 : o.hud_below; if (hd != W.hud_dy) { W.hud_dy = hd; W.rebuild = 1; } }
        double pitch_r = front.loaded ? front.scene.pitch_deg * M_PI / 180 : 0;                  /* the scene's own tilt: the picture's horizon made level */

        /* take a finished scene from the worker */
        SDL_LockMutex(W.mu);
        if (W.ready) {
            gl_scene_free(&front); front = W.back; memset(&W.back, 0, sizeof W.back); shown_frame = W.back_frame; table_name = W.back_table; cur_scene = W.back_scene; cur_sim = W.back_sim; W.ready = 0;
            SDL_UnlockMutex(W.mu);
            front.no_rulers = 1; gl_scene_upload(&front);
            glBindBuffer(GL_ARRAY_BUFFER, gfx.vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(front.nverts ? front.nverts : 1) * sizeof(Vtx), front.verts, GL_DYNAMIC_DRAW);   /* the scene's geometry */
        } else SDL_UnlockMutex(W.mu);

        M4 view[2], proj[2]; int have_views = 0; XrPosef head = { { 0, 0, 0, 1 }, { 0, 0, 0 } }; int have_head = 0;
        double ray_o[3] = { 0, 0, 0 }, ray_d[3] = { 0, 0, -1 }; int have_ray = 0; double ctl_pos[3] = { 0, 0, 0 };
        XrFrameState fs = { XR_TYPE_FRAME_STATE }; int trg[2] = { 0, 0 };
        if (!o.no_vr) {
            xr_events(&xr); if (xr.exit_requested) { fprintf(stderr, "pbvr: the OpenXR session asked to exit\n"); break; }
            if (!xr.running) { SDL_Delay(20); continue; }
            XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO }; { XrResult wr = xrWaitFrame(xr.sess, &wi, &fs); if (XR_FAILED(wr)) { fprintf(stderr, "pbvr: xrWaitFrame failed (%d), stopping\n", (int)wr); break; } }
            XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO }; xrBeginFrame(xr.sess, &bi);
            xr.pred = fs.predictedDisplayTime;
            /* input */
            XrActiveActionSet as = { xr.set, XR_NULL_PATH }; XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO }; sy.countActiveActionSets = 1; sy.activeActionSets = &as; xrSyncActions(xr.sess, &sy);
            int cn = 0, st = 0;
            for (int h = 0; h < 2; h++) {
                XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.subactionPath = xr.hand[h];
                XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT }; gi.action = xr.a_trigger; xrGetActionStateFloat(xr.sess, &gi, &f); trg[h] = f.isActive && f.currentState > 0.55f;
                XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN }; gi.action = xr.a_coin; xrGetActionStateBoolean(xr.sess, &gi, &b); in_a[h] = b.isActive && b.currentState; if (in_a[h]) cn = 1;
                gi.action = xr.a_start; b = (XrActionStateBoolean){ XR_TYPE_ACTION_STATE_BOOLEAN }; xrGetActionStateBoolean(xr.sess, &gi, &b); in_b[h] = b.isActive && b.currentState; if (in_b[h]) st = 1;
                f = (XrActionStateFloat){ XR_TYPE_ACTION_STATE_FLOAT }; gi.action = xr.a_squeeze; xrGetActionStateFloat(xr.sess, &gi, &f); in_sq[h] = f.isActive && f.currentState > 0.6f;
                XrActionStateVector2f v2 = { XR_TYPE_ACTION_STATE_VECTOR2F }; gi.action = xr.a_stick; xrGetActionStateVector2f(xr.sess, &gi, &v2); stick[h] = v2.isActive ? v2.currentState.y : 0; stick_x[h] = v2.isActive ? v2.currentState.x : 0;
                if (trg[h] && !prev_trig[h]) active_hand = h;
            }
            if (trg[active_hand] == 0 && trg[1 - active_hand]) active_hand = 1 - active_hand;
            { XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
              gi.action = xr.a_pause; xrGetActionStateBoolean(xr.sess, &gi, &b); int p = b.isActive && b.currentState; if (p && !prev_pause) paused = !paused; prev_pause = p;
              gi.action = xr.a_recenter; b = (XrActionStateBoolean){ XR_TYPE_ACTION_STATE_BOOLEAN }; xrGetActionStateBoolean(xr.sess, &gi, &b); p = b.isActive && b.currentState; if (p && !prev_rec) { if (paused) { adj_target = (adj_target + 1) % 5; need_msg_adj = 1; } else need_recenter = 1; } prev_rec = p; }
            if (!paused && trg[active_hand] && !trig) {                                                 /* a short buzz on each shot */
                XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = xr.a_haptic; hi.subactionPath = xr.hand[active_hand];
                XrHapticVibration vb = { XR_TYPE_HAPTIC_VIBRATION }; vb.duration = 25000000; vb.frequency = XR_FREQUENCY_UNSPECIFIED; vb.amplitude = 0.8f; xrApplyHapticFeedback(xr.sess, &hi, (XrHapticBaseHeader *)&vb);
            }
            prev_trig[0] = trg[0]; prev_trig[1] = trg[1]; trig = trg[active_hand]; coin = cn | kcoin; start = st | kstart;
            if (paused) trig = coin = start = 0;                       /* the game is stopped; the buttons edit instead */
            { static double both_since = 0; if (!paused && in_sq[0] && in_sq[1]) { if (both_since == 0) both_since = now_s(); else if (now_s() - both_since > 0.8) { W.auto_on = !W.auto_on; both_since = -1e9; snprintf(msg, sizeof msg, "auto-player %s", W.auto_on ? "ON" : "off"); msg_until = now_s() + 3; } }
              else if (!(in_sq[0] && in_sq[1])) both_since = 0; }
            /* views */
            XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fs.predictedDisplayTime; li.space = xr.local;
            XrViewState vs = { XR_TYPE_VIEW_STATE }; for (int i = 0; i < 2; i++) xr.views[i].type = XR_TYPE_VIEW; uint32_t nv = 0;
            if (XR_SUCCEEDED(xrLocateViews(xr.sess, &li, &vs, 2, &nv, xr.views)) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && nv == 2) {
                have_views = 1; head.position.x = (xr.views[0].pose.position.x + xr.views[1].pose.position.x) / 2; head.position.y = (xr.views[0].pose.position.y + xr.views[1].pose.position.y) / 2;
                head.position.z = (xr.views[0].pose.position.z + xr.views[1].pose.position.z) / 2; head.orientation = xr.views[0].pose.orientation; have_head = 1;
            }
            if (have_head && need_recenter) {                                              /* the arcade eye = head position now, facing where the head faces (yaw only) */
                M4 h = m4_from_pose(head); yaw0 = atan2(h.m[8], h.m[10]);
                origin[0] = head.position.x; origin[1] = head.position.y; origin[2] = head.position.z; need_recenter = 0;
            }
            /* aim ray from the gun hand, in scene coordinates */
            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION }; XrPosef aimp = head; int aim_ok = 0;
            if (!o.gaze && XR_SUCCEEDED(xrLocateSpace(xr.aim_space[active_hand], xr.local, fs.predictedDisplayTime, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { aimp = loc.pose; aim_ok = 1; }
            else if (have_head) { aimp = head; aim_ok = 1; }                                  /* no tracked controller: aim with the head */
            if (aim_ok) {
                M4 c = m4_from_pose(aimp); double deg = M_PI / 180;
                if (o.gun_pitch != 0 || o.aim_pitch != 0) { double a = (o.gun_pitch + o.aim_pitch) * deg, cs = cos(a), sn = sin(a); M4 rp = m4_id(); rp.m[5] = cs; rp.m[6] = sn; rp.m[9] = -sn; rp.m[10] = cs; c = m4_mul(c, rp); }
                if (o.aim_yaw != 0) { double a = o.aim_yaw * deg, cs = cos(a), sn = sin(a); M4 ry = m4_id(); ry.m[0] = cs; ry.m[2] = -sn; ry.m[8] = sn; ry.m[10] = cs; c = m4_mul(c, ry); }
                double o_phys[3] = { c.m[12], c.m[13], c.m[14] }, d_phys[3] = { -c.m[8], -c.m[9], -c.m[10] };
                M4 to_scene = m4_mul(m4_model_inv(zs, pitch_r), m4_mul(m4_trans(0, 0, -jump), m4_rigid_inverse(m4_yaw_pos(yaw0, origin))));
                m4_point(to_scene, o_phys, ray_o); m4_dir(to_scene, d_phys, ray_d); have_ray = 1; memcpy(ctl_pos, o_phys, sizeof ctl_pos);
            }
            if (have_views) for (int e = 0; e < 2; e++) {
                M4 scene_to_phys = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r)));
                view[e] = m4_mul(m4_rigid_inverse(m4_from_pose(xr.views[e].pose)), scene_to_phys); proj[e] = m4_proj(xr.views[e].fov, 0.05, 600.0);
            }
        }

        /* aim -> arcade pixel (the same snapping as pbdesk) */
        double hit_phys[3]; int have_hit = 0, hit_item = -1; static double dbg_rx, dbg_ry;
        if (have_ray && front.loaded) {
            Nb1AimSnap r;
            if (nb1_aim_snap(&front.scene, front.ids, ray_o, ray_d, 24.0, &r)) {
                dbg_rx = r.raw_sx; dbg_ry = r.raw_sy; sx = r.a.sx; sy = r.a.sy; hit_item = r.a.item; M4 scene_to_phys = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r))); m4_point(scene_to_phys, r.a.hit, hit_phys); have_hit = 1;
            }
        }
        if (W.auto_on && W.bot_valid && !paused) { sx = W.bot_sx; sy = W.bot_sy; trig = W.bot_trig; if (W.bot_coin) coin = 1; if (W.bot_start) start = 1; }      /* the auto-player plays: it aims and fires, and puts in the credits */
        double t = now_s(), dt = t - last_t; if (dt > 0.1) dt = 0.1; last_t = t;
        if (tour) { af++; if (af % 60 == 30) shot_req = 1; if (af % 60 == 0) nav = 1; if (af > 60 * atoi(getenv("PBVR_TOUR")) + 40) { fprintf(stderr, "pbvr: tour finished\n"); running = 0; } }   /* PBVR_TOUR=n: visit n scenes, a screenshot each */
        if (getenv("PBVR_AUTOTEST")) {                                                    /* PBVR_AUTOTEST: a scripted editing session, with screenshots PBVR_SHOT.N.ppm */
            af++; kb_ls = 0;
            if (af == 150) paused = 1;
            if (af == 190) inj_sel = 1;
            if (af > 200 && af < 260) kb_ls = 1;                           /* scale the layer up */
            if (af == 270) shot_req = 1;
            if (af == 280) inj_a = 1;                                      /* jump to it */
            if (af == 300) shot_req = 1;
            if (af == 310) inj_b = 1;                                      /* save */
            if (af == 320) inj_a = 1;                                      /* and back */
            if (af == 340) shot_req = 1;
            if (af == 360) { inj_sq = 1; }
            if (af == 380) inj_sel = 1;
            if (af > 390 && af < 440) kb_ls = -1;
            if (af == 450) shot_req = 1;
            if (af == 460) inj_b = 1;
            if (af == 465) yaw0 -= 0.95;                                    /* "turn" left: the controls panel comes to the front */
            if (af == 475) shot_req = 1;
            if (af == 480) yaw0 += 1.9;                                     /* and right: the edits panel */
            if (af == 490) shot_req = 1;
            if (af == 500) { fprintf(stderr, "pbvr: autotest finished\n"); running = 0; }
        }
        { float fx = fabsf(stick_x[0]) > fabsf(stick_x[1]) ? stick_x[0] : stick_x[1]; int fl = fx > 0.8f ? 1 : fx < -0.8f ? -1 : 0;      /* flick a stick sideways: next / previous scene */
          if (fl && !prev_flick) nav = fl; prev_flick = fl; }
        if (nav && scn) {
            scn_idx = ((scn_idx + nav) % nscn + nscn) % nscn; nav = 0;
            snprintf(scene_desc, sizeof scene_desc, "SCENE %d  %s  (%d of %d)%s%s", scn[scn_idx].id, scn[scn_idx].family, scn_idx + 1, nscn, scn[scn_idx].label[0] ? "  " : "", scn[scn_idx].label);
            if (!o.live) W.goto_idx = scn_idx; else snprintf(load_name, sizeof load_name, "%s@%d", scn[scn_idx].state, ++load_seq);
            snprintf(msg, sizeof msg, "%s", scene_desc); msg_until = now_s() + 3; has_sel = 0; jump = 0;
        } else nav = 0;
        /* ---- pause-mode editing */
        if (!paused) jump = 0;
        else if (!o.no_vr) {
            int sel_edge = (trg[active_hand] && !prev_seltrig) || inj_sel, sq_edge = (in_sq[0] && !prev_sq[0]) || (in_sq[1] && !prev_sq[1]) || inj_sq;
            int a_edge = (in_a[1] && !prev_ba) || inj_a, b_edge = (in_b[1] && !prev_bb) || inj_b, x_edge = (in_a[0] && !prev_bx) || inj_x, y_edge = (in_b[0] && !prev_by) || inj_y, changed = 0;
            if (sel_edge && hit_item >= 0 && front.loaded) {
                const Nb1Item *it = &front.scene.items[hit_item];
                sel_item = (SelItem){ it->kind, it->id, it->pri, it->has_key, it->key };
                if (spec_from_item(&sel_item, scope, &sel)) has_sel = 1;
            }
            if (sq_edge && has_sel) { scope ^= 1; spec_from_item(&sel_item, scope, &sel); }
            double ls = stick[0] + kb_ls, rs = stick[1] + kb_rs;
            if (has_sel && fabs(ls) > 0.15) {                                          /* left stick up = push the layer away (bigger), down = pull it closer (smaller) */
                SDL_LockMutex(W.mu);
                OvEntry *e = ov_find(W.ov, W.nov, cur_scene, &sel);
                if (!e) { if (W.nov == W.capov) { W.capov = W.capov ? W.capov * 2 : 16; W.ov = realloc(W.ov, (size_t)W.capov * sizeof *W.ov); } e = &W.ov[W.nov++]; e->scene = cur_scene; e->s = sel; e->s.k = 1; }
                e->s.k *= exp(dt * ls * 0.6); if (e->s.k < 0.1) e->s.k = 0.1; if (e->s.k > 6) e->s.k = 6;
                SDL_UnlockMutex(W.mu); changed = 1;
            }
            if (need_msg_adj) { snprintf(msg, sizeof msg, "right stick now adjusts: %s", ADJ_NAME[adj_target]); msg_until = t + 3; need_msg_adj = 0; }
            if (fabs(rs) > 0.15) {
                if (adj_target == 0) { o.vscale *= exp(dt * rs * 0.7); if (o.vscale < 0.2) o.vscale = 0.2; if (o.vscale > 6) o.vscale = 6; if (jump != 0) jump = 0; }
                else if (adj_target == 4) { o.hud_flat = 0; o.hud_below += rs * dt * 60; if (o.hud_below < 0) o.hud_below = 0; if (o.hud_below > 160) o.hud_below = 160; }
                else if (front.loaded) {                                                   /* the world's tilt, floor, ceiling: per scene, rebuilt by the worker */
                    SDL_LockMutex(W.mu); SurAdj *ad = NULL;
                    for (int i = 0; i < W.nadj; i++) if (W.adj[i].scene == cur_scene) ad = &W.adj[i];
                    if (!ad) { if (W.nadj == W.capadj) { W.capadj = W.capadj ? W.capadj * 2 : 8; W.adj = realloc(W.adj, (size_t)W.capadj * sizeof *W.adj); } ad = &W.adj[W.nadj++]; memset(ad, 0, sizeof *ad); ad->scene = cur_scene; }
                    if (adj_target == 1) { if (!ad->has[0]) { ad->has[0] = 1; ad->pitch = front.scene.pitch_deg; } ad->pitch += rs * dt * 6; if (ad->pitch < -45) ad->pitch = -45; if (ad->pitch > 45) ad->pitch = 45; changed = 1; }
                    else if (adj_target == 2 && !isnan(front.scene.ground_y)) { if (!ad->has[1]) { ad->has[1] = 1; ad->ground = front.scene.ground_y; } ad->ground += rs * dt * 0.5; if (ad->ground > -0.2) ad->ground = -0.2; if (ad->ground < -12) ad->ground = -12; changed = 1; }
                    else if (adj_target == 3 && !isnan(front.scene.ceil_y)) { if (!ad->has[2]) { ad->has[2] = 1; ad->ceil = front.scene.ceil_y; } ad->ceil += rs * dt * 0.5; if (ad->ceil < 0.2) ad->ceil = 0.2; if (ad->ceil > 12) ad->ceil = 12; changed = 1; }
                    SDL_UnlockMutex(W.mu);
                }
            }
            if (x_edge && has_sel) {                                                    /* reset the selected group */
                SDL_LockMutex(W.mu); for (int i = 0; i < W.nov; i++) if (W.ov[i].scene == cur_scene && spec_same(&W.ov[i].s, &sel)) { W.ov[i] = W.ov[--W.nov]; break; } SDL_UnlockMutex(W.mu);
                changed = 1; snprintf(msg, sizeof msg, "reset %s", "the selected group"); msg_until = t + 3;
            }
            if (y_edge) { SDL_LockMutex(W.mu); for (int i = W.nov - 1; i >= 0; i--) if (W.ov[i].scene == cur_scene) W.ov[i] = W.ov[--W.nov]; SDL_UnlockMutex(W.mu); changed = 1; snprintf(msg, sizeof msg, "reset every edit of this scene"); msg_until = t + 3; }
            if (a_edge) {                                                               /* jump to the selected layer at its real size, or back */
                if (jump != 0) { jump = 0; snprintf(msg, sizeof msg, "back at the playing area"); msg_until = t + 3; }
                else if (has_sel) { double zm; if (group_z(&front, &sel, &zm)) { double zp = zm * zs; jump = zp > o.jump_dist ? zp - o.jump_dist : 0; snprintf(msg, sizeof msg, "jumped to the layer (%.1f m away at real size); A to go back", zp > o.jump_dist ? o.jump_dist : zp); msg_until = t + 3; } }
                else { snprintf(msg, sizeof msg, "pick a layer first (trigger)"); msg_until = t + 3; }
            }
            if (b_edge) {
                SDL_LockMutex(W.mu); int nsaved = save_edits(o.edits_out, &W, &front, cur_scene, cur_sim, table_name, shown_frame, &o, zs); SDL_UnlockMutex(W.mu);
                const char *s1 = strrchr(o.edits_out, '/'), *s2 = strrchr(o.edits_out, '\\'), *bn = s1 > s2 ? s1 : s2; bn = bn ? bn + 1 : o.edits_out;
                if (nsaved >= 0) snprintf(msg, sizeof msg, "saved %d edit(s) to %.100s", nsaved, bn); else snprintf(msg, sizeof msg, "could not write %.100s", bn);
                msg_until = t + 4;
            }
            if (changed) W.rebuild = 1;
        }
        inj_sel = inj_sq = inj_a = inj_b = inj_x = inj_y = 0; prev_seltrig = trg[active_hand]; prev_sq[0] = in_sq[0]; prev_sq[1] = in_sq[1]; prev_ba = in_a[1]; prev_bb = in_b[1]; prev_bx = in_a[0]; prev_by = in_b[0];
        if (o.ipc[0] && t - last_aim_write > 1.0 / 120) {
            FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d %d%s%s\n", ++seq, sx, sy, trig, coin, start, paused, load_name[0] ? " " : "", load_name); fclose(f); replace_file(aim_tmp, aim_path); }
            last_aim_write = t;
        }

        /* the editor's text */
        if (paused) {
            char l[200]; double zm = 0; int ni = has_sel ? group_z(&front, &sel, &zm) : 0; double kk = 1;
            if (has_sel) { SDL_LockMutex(W.mu); const OvEntry *e = ov_find(W.ov, W.nov, cur_scene, &sel); if (e) kk = e->s.k; SDL_UnlockMutex(W.mu); }
            if (scn) snprintf(l, sizeof l, "%s   %s   table: %s", scn && !o.live ? "FROZEN" : "PAUSED", scene_desc, table_name ? table_name : "default"); else snprintf(l, sizeof l, "PAUSED   scene #%d (%.0f%% art match)   table: %s   frame %u", cur_scene, cur_sim * 100, table_name ? table_name : "default", shown_frame); panel_set(&panel, 0, l);
            if (has_sel && ni) { char d[80]; spec_desc(&sel, d, sizeof d); snprintf(l, sizeof l, "SELECTED: %s (%s, %d item%s)   depth %.1f m   scale x%.2f", d, scope ? "this sprite type" : "whole layer", ni, ni == 1 ? "" : "s", zm * zs, kk); }
            else snprintf(l, sizeof l, "Point at a layer and pull the trigger to select it");
            panel_set(&panel, 1, l);
            snprintf(l, sizeof l, "%cviewport x%.2f (%.2f m)   %ctilt %.1f deg   %cfloor %s   %cceiling %s   %cHUD %.0f px below   %s", adj_target == 0 ? '>' : ' ', o.vscale, vw_m, adj_target == 1 ? '>' : ' ', front.loaded ? front.scene.pitch_deg : 0.0, adj_target == 2 ? '>' : ' ', front.loaded && !isnan(front.scene.ground_y) ? (snprintf(adjf, sizeof adjf, "%.2f m", front.scene.ground_y), adjf) : "-", adj_target == 3 ? '>' : ' ', front.loaded && !isnan(front.scene.ceil_y) ? (snprintf(adjc, sizeof adjc, "%.2f m", front.scene.ceil_y), adjc) : "-", adj_target == 4 ? '>' : ' ', o.hud_flat ? 0.0 : o.hud_below, jump != 0 ? "JUMPED" : ""); panel_set(&panel, 2, l);
            panel_set(&panel, 3, t < msg_until ? msg : (scn ? "stick left / right: previous / next scene" : ""));
            /* controls on the left (turn your head left), this scene's edits on the right */
            static const char *help[] = { "CONTROLS (paused)", "", "trigger          select a layer", "grip             layer / sprite type", "left stick       scale the layer", "                 (up = farther, bigger)",
                                          "right stick      adjust (see >)", "A                jump to layer / back", "B                save the edits", "X                reset selected", "Y                reset all of this scene", "left stick click resume the game", "right stick click next adjust:", "viewport / tilt / floor / ceiling / HUD" };
            for (int i = 0; i < PANEL_LINES; i++) panel_set(&side_l, i, i < 14 ? help[i] : "");
            int row = 0; char r[200];
            snprintf(r, sizeof r, "EDITS IN SCENE #%d", cur_scene); panel_set(&side_r, row++, r); panel_set(&side_r, row++, "");
            SDL_LockMutex(W.mu);
            for (int i = 0; i < W.nov && row < PANEL_LINES - 2; i++) if (W.ov[i].scene == cur_scene) {
                char d[80]; double zq = 0; spec_desc(&W.ov[i].s, d, sizeof d); group_z(&front, &W.ov[i].s, &zq);
                snprintf(r, sizeof r, "%c %s  x%.2f  %.1f m", (has_sel && spec_same(&W.ov[i].s, &sel)) ? '>' : ' ', d, W.ov[i].s.k, zq * zs); panel_set(&side_r, row++, r);
            }
            SDL_UnlockMutex(W.mu);
            if (row == 2) panel_set(&side_r, row++, "(none yet)");
            { const char *s1 = strrchr(o.edits_out, '/'), *s2 = strrchr(o.edits_out, '\\'), *bn = s1 > s2 ? s1 : s2; snprintf(r, sizeof r, "B saves to %.150s", bn ? bn + 1 : o.edits_out); } panel_set(&side_r, row++, ""); panel_set(&side_r, row++, r);
            while (row < PANEL_LINES) panel_set(&side_r, row++, "");
        } else if (t < msg_until) {                                           /* a short note while playing: the scene just jumped to */
            panel_set(&panel, 0, scn ? scene_desc : ""); panel_set(&panel, 1, ""); panel_set(&panel, 2, ""); panel_set(&panel, 3, msg);
        }
        { char nm[320]; if (scn) snprintf(nm, sizeof nm, "%s   table: %s", scene_desc, table_name ? table_name : "default"); else snprintf(nm, sizeof nm, "SCENE #%d (%.0f%% art match)   table: %s", cur_scene, cur_sim * 100, table_name ? table_name : "default"); if (W.auto_on) { SDL_LockMutex(W.mu); size_t L = strlen(nm); snprintf(nm + L, sizeof nm - L, "     %s", W.bot_status[0] ? W.bot_status : "auto: starting"); SDL_UnlockMutex(W.mu); }
            panel_set(&namep, 0, o.no_labels ? "" : nm); }
        /* render */
        if (!o.no_vr) {
            XrCompositionLayerProjectionView pv[2]; XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            if (have_views && fs.shouldRender) {
                for (int e = 0; e < 2; e++) {
                    Eye *E = &xr.eye[e]; uint32_t idx = 0; XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; xrAcquireSwapchainImage(E->sc, &ai, &idx);
                    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(E->sc, &wi);
                    pBindFramebuffer(GL_FRAMEBUFFER, E->fbo); pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, E->img[idx].image, 0);
                    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, E->depth);
                    glViewport(0, 0, E->w, E->h); if (front.loaded && front.scene.has_clear) glClearColor((float)(front.scene.clear[0] / 255.0), (float)(front.scene.clear[1] / 255.0), (float)(front.scene.clear[2] / 255.0), 1); else glClearColor(0.02f, 0.02f, 0.03f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    M4 mvp = m4_mul(proj[e], view[e]);
                    if (front.loaded) draw_scene(&gfx, &front, &mvp);
                    if (paused || t < msg_until) {
                        M4 pv_ = m4_mul(proj[e], m4_rigid_inverse(m4_from_pose(xr.views[e].pose))); M4 s2p = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r)));
                        if (paused && has_sel && front.loaded) for (int i = 0; i < front.scene.n; i++) {          /* outline every item of the selected group */
                            const Nb1Item *it = &front.scene.items[i]; if (!spec_matches(&sel, it)) continue;
                            double Pq[16][3]; int mm = item_polygon(it, front.scene.far, Pq); if (mm < 3) continue;
                            float seg[96]; int nv = 0;
                            for (int k = 0; k < mm && nv < 30; k++) for (int q = 0; q < 2; q++) { double o3[3]; m4_point(s2p, Pq[(k + q) % mm], o3); seg[nv * 3] = (float)o3[0]; seg[nv * 3 + 1] = (float)o3[1]; seg[nv * 3 + 2] = (float)o3[2]; nv++; }
                            draw_lines(&gfx, &pv_, seg, nv, 1.0f, 0.85f, 0.1f);
                        }
                        panel_draw(&gfx, &panel, &pv_, m4_yaw_pos(yaw0, origin), o.sdist, -(vw_m * 224.0 / 288.0) / 2 - (o.hud_flat || o.no_labels ? 0 : (o.hud_below + 88) * vw_m / 288.0) - 0.02, vw_m / 480.0, 0);
                        if (paused) panel_draw(&gfx, &side_l, &pv_, m4_yaw_pos(yaw0 + 0.95, origin), 1.0, 0.16, 0.0013, 1);       /* turn left: the controls */
                        if (paused) panel_draw(&gfx, &side_r, &pv_, m4_yaw_pos(yaw0 - 0.95, origin), 1.0, 0.16, 0.0013, 1);       /* turn right: this scene's edits */
                    }
                    if (!o.no_labels && front.loaded) {                               /* scene name under the picture; small names on the HUD sprites (for notes while recording) */
                        M4 pv_ = m4_mul(proj[e], m4_rigid_inverse(m4_from_pose(xr.views[e].pose))); M4 s2p = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r)));
                        double hb = o.hud_flat ? 0.0 : o.hud_below;
                        panel_draw(&gfx, &namep, &pv_, m4_yaw_pos(yaw0, origin), o.sdist, -(vw_m * 224.0 / 288.0) / 2 - (hb + 66) * vw_m / 288.0, vw_m / 400.0, 0);
                        int nl = 0, stack = 0; double rc[48][4];                                  /* placed labels (arcade px): a new one is pushed down until it clears them */
                        for (int i = 0; i < front.scene.n && nl < 48; i++) {
                            const Nb1Item *it = &front.scene.items[i]; if (!it->hud || (it->kind != NB1_SPRITE && it->kind != NB1_TILEMAP)) continue;
                            char lt[48]; double px0, py0; double zz = it->z, F = NB1_FOCAL;
                            if (it->kind == NB1_TILEMAP) { snprintf(lt, sizeof lt, "tilemap L%d", it->id); px0 = NB1_W - 64; py0 = NB1_H + hb + 2 + 9.0 * stack++; }
                            else { if (it->has_key) snprintf(lt, sizeof lt, "S%d #%d", it->pri, it->key); else snprintf(lt, sizeof lt, "S%d", it->pri); double Pq[16][3]; int mm = item_polygon(it, front.scene.far, Pq); if (mm < 3) continue;
                                   double mx = 1e9, my = 1e9; for (int k = 0; k < mm; k++) { if (Pq[k][0] < mx) mx = Pq[k][0]; double yq = Pq[k][1] - hb * it->z / F; if (yq < my) my = yq; }   /* left edge, bottom edge (after the HUD shift) */
                                   px0 = NB1_W / 2.0 + mx * F / it->z; py0 = NB1_H / 2.0 - my * F / it->z + 1; }
                            LabelTex *lt_ = label_get(&lc, lt); if (!lt_) continue;
                            { double w_ = lt_->w * 0.8, h_ = lt_->h * 0.8; for (int again = 1, g = 0; again && g < 12; g++) { again = 0; for (int j = 0; j < nl; j++) if (px0 < rc[j][0] + rc[j][2] && px0 + w_ > rc[j][0] && py0 < rc[j][1] + rc[j][3] && py0 + h_ > rc[j][1]) { py0 = rc[j][1] + rc[j][3]; again = 1; } } rc[nl][0] = px0; rc[nl][1] = py0; rc[nl][2] = w_; rc[nl][3] = h_; }
                            double k_ = zz / F, tw = lt_->w * 0.8 * k_, th = lt_->h * 0.8 * k_, x0 = (px0 - NB1_W / 2.0) * k_, y0 = (NB1_H / 2.0 - py0) * k_;
                            double cx[4] = { x0, x0 + tw, x0 + tw, x0 }, cy[4] = { y0, y0, y0 - th, y0 - th }; static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }; Vtx c[4], q[6];
                            for (int k = 0; k < 4; k++) { double v[3] = { cx[k], cy[k], -zz }, o3[3]; m4_point(s2p, v, o3); c[k] = (Vtx){ (float)o3[0], (float)o3[1], (float)o3[2], uv[k][0], uv[k][1] }; }
                            q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = c[0]; q[4] = c[2]; q[5] = c[3];
                            float m[16]; for (int k = 0; k < 16; k++) m[k] = (float)pv_.m[k];
                            glDisable(GL_DEPTH_TEST); glUseProgram(gfx.prog); glUniformMatrix4fv(gfx.uMVP, 1, GL_FALSE, m); glUniform1i(gfx.uTex, 0); glUniform1i(gfx.uMode, 4);
                            glBindVertexArray(gfx.line_vao); glBindBuffer(GL_ARRAY_BUFFER, gfx.line_vbo); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW); glBindTexture(GL_TEXTURE_2D, lt_->tex); glDrawArrays(GL_TRIANGLES, 0, 6);
                            glBindVertexArray(gfx.vao); glBindBuffer(GL_ARRAY_BUFFER, gfx.vbo); glEnable(GL_DEPTH_TEST); nl++;
                        }
                    }
                    if (!o.no_brackets && front.loaded) {                              /* white corner marks round the play window, like a camera viewfinder */
                        M4 pv_ = m4_mul(proj[e], m4_rigid_inverse(m4_from_pose(xr.views[e].pose))); M4 s2p = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r)));
                        double zb = o.sdist / zs, Fz = zb / NB1_FOCAL, L = 28; float seg[8 * 2 * 3]; int nv = 0;
                        for (int cxi = 0; cxi < 2; cxi++) for (int cyi = 0; cyi < 2; cyi++) {
                            double px = cxi ? NB1_W : 0, py = cyi ? NB1_H : 0, dx = cxi ? -L : L, dy = cyi ? -L : L;
                            double pts[3][2] = { { px + dx, py }, { px, py }, { px, py + dy } };
                            for (int q = 0; q < 2; q++) for (int k = 0; k < 2; k++) {
                                double P[3] = { (pts[q + k][0] - NB1_W / 2.0) * Fz, (NB1_H / 2.0 - pts[q + k][1]) * Fz, -zb }, O3[3]; m4_point(s2p, P, O3);
                                seg[nv * 3] = (float)O3[0]; seg[nv * 3 + 1] = (float)O3[1]; seg[nv * 3 + 2] = (float)O3[2]; nv++;
                            }
                        }
                        draw_lines(&gfx, &pv_, seg, nv, 1.0f, 1.0f, 1.0f);
                    }
                    if (have_ray) {                                                   /* laser and a small cross where it lands (physical coordinates) */
                        M4 pv_ = m4_mul(proj[e], m4_rigid_inverse(m4_from_pose(xr.views[e].pose)));
                        double tip[3]; if (have_hit) memcpy(tip, hit_phys, sizeof tip); else { M4 sp = m4_mul(m4_yaw_pos(yaw0, origin), m4_mul(m4_trans(0, 0, jump), m4_model(zs, pitch_r))); double far_s[3] = { ray_o[0] + ray_d[0] * 6, ray_o[1] + ray_d[1] * 6, ray_o[2] + ray_d[2] * 6 }; m4_point(sp, far_s, tip); }
                        float L[6] = { (float)ctl_pos[0], (float)ctl_pos[1], (float)ctl_pos[2], (float)tip[0], (float)tip[1], (float)tip[2] };
                        draw_lines(&gfx, &pv_, L, 2, trig ? 1.0f : 0.2f, trig ? 0.9f : 0.9f, trig ? 0.2f : 0.3f);
                        if (have_hit) { const float s = 0.02f; float X[8 * 3] = { (float)tip[0] - s, (float)tip[1], (float)tip[2], (float)tip[0] + s, (float)tip[1], (float)tip[2], (float)tip[0], (float)tip[1] - s, (float)tip[2], (float)tip[0], (float)tip[1] + s, (float)tip[2] };
                                         draw_lines(&gfx, &pv_, X, 4, 1.0f, 0.3f, 0.3f); }
                    }
                    if (e == 0 && getenv("PBVR_SHOT") && front.loaded && (autotest ? shot_req : ++shot_wait == 30)) {            /* PBVR_SHOT=file.ppm: save the left eye once a scene is showing (testing) */
                        uint8_t *px = malloc((size_t)E->w * E->h * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, E->w, E->h, GL_RGB, GL_UNSIGNED_BYTE, px);
                        char shot_name[600]; if (autotest) snprintf(shot_name, sizeof shot_name, "%s.%d.ppm", getenv("PBVR_SHOT"), shot_n++); else snprintf(shot_name, sizeof shot_name, "%s", getenv("PBVR_SHOT")); shot_req = 0;
                        FILE *sf = fopen(shot_name, "wb"); if (sf) { fprintf(sf, "P6\n%d %d\n255\n", E->w, E->h); for (int y = E->h - 1; y >= 0; y--) fwrite(px + (size_t)y * E->w * 3, 1, (size_t)E->w * 3, sf); fclose(sf); fprintf(stderr, "pbvr: saved %s\n", shot_name); }
                        free(px);
                    }
                    if (e == 0) {                                                     /* mirror window = left eye */
                        int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
                        pBindFramebuffer(GL_READ_FRAMEBUFFER, E->fbo); pBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); pBlitFramebuffer(0, 0, E->w, E->h, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                        SDL_GL_SwapWindow(win);
                    }
                    pBindFramebuffer(GL_FRAMEBUFFER, 0);
                    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; xrReleaseSwapchainImage(E->sc, &ri);
                    pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }; pv[e].pose = xr.views[e].pose; pv[e].fov = xr.views[e].fov;
                    pv[e].subImage.swapchain = E->sc; pv[e].subImage.imageRect.extent.width = E->w; pv[e].subImage.imageRect.extent.height = E->h;
                }
                layer.space = xr.local; layer.viewCount = 2; layer.views = pv;
            }
            const XrCompositionLayerBaseHeader *layers[1] = { (const XrCompositionLayerBaseHeader *)&layer };
            XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO }; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            ei.layerCount = (have_views && fs.shouldRender) ? 1 : 0; ei.layers = layers; xrEndFrame(xr.sess, &ei);
        } else {                                                                      /* --no-vr: a flat window so the pipeline can be checked without a headset */
            int ww, wh; SDL_GetWindowSize(win, &ww, &wh); glViewport(0, 0, ww, wh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            SDL_GL_SwapWindow(win); SDL_Delay(8);
        }
        fps_n++; if (t - fps_t > 2.0) {
            char title[200]; snprintf(title, sizeof title, "Point Blank VR   %.0f fps   scene frame %u   table %s   aim %.0f,%.0f%s   depth x%.2f", fps_n / (t - fps_t), shown_frame, table_name ? table_name : "default", sx, sy, paused ? "   PAUSED" : "", zs);
            SDL_SetWindowTitle(win, title); fprintf(stderr, "pbvr: %s\n", title);
            if (getenv("PBVR_DEBUG")) {
                fprintf(stderr, "  head %.3f %.3f %.3f yaw0 %.3f origin %.3f %.3f %.3f ray_o %.3f %.3f %.3f ray_d %.3f %.3f %.3f raw %.1f %.1f\n", head.position.x, head.position.y, head.position.z, yaw0, origin[0], origin[1], origin[2], ray_o[0], ray_o[1], ray_o[2], ray_d[0], ray_d[1], ray_d[2], dbg_rx, dbg_ry);
            }
            if (!front.loaded) fprintf(stderr, "pbvr: no game state received yet from %s - is MAME running with pbplay.lua and PB_IPC set to that folder? (the headset stays empty until it is)\n", o.ipc);
            fps_t = t; fps_n = 0;
        }
    }
    fprintf(stderr, "pbvr: leaving the main loop\n"); if (o.bot_stats[0]) nb1_bot_save_stats(W.bot, o.bot_stats); W.quit = 1; SDL_WaitThread(thr, NULL);
    if (!o.no_vr && xr.sess) { xrDestroySession(xr.sess); xrDestroyInstance(xr.inst); }
    return 0;
}
