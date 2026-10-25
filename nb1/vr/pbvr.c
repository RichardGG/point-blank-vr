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
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateFloat) X(xrGetActionStateBoolean) X(xrApplyHapticFeedback) X(xrResultToString)
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
static M4 m4_scale_z(double zs) { M4 r = m4_id(); r.m[10] = zs; return r; }
static M4 m4_proj(XrFovf f, double n, double fa) {
    double l = tan(f.angleLeft), r = tan(f.angleRight), u = tan(f.angleUp), d = tan(f.angleDown); M4 p; memset(&p, 0, sizeof p);
    p.m[0] = 2 / (r - l); p.m[5] = 2 / (u - d); p.m[8] = (r + l) / (r - l); p.m[9] = (u + d) / (u - d);
    p.m[10] = -(fa + n) / (fa - n); p.m[11] = -1; p.m[14] = -2 * fa * n / (fa - n); return p;
}
static void m4_point(M4 a, const double v[3], double out[3]) { for (int i = 0; i < 3; i++) out[i] = a.m[i] * v[0] + a.m[4 + i] * v[1] + a.m[8 + i] * v[2] + a.m[12 + i]; }
static void m4_dir(M4 a, const double v[3], double out[3]) { for (int i = 0; i < 3; i++) out[i] = a.m[i] * v[0] + a.m[4 + i] * v[1] + a.m[8 + i] * v[2]; }

/* ------------------------------------------------------------------ options */
typedef struct {
    char roms[512], ipc[512], table[512], tables[512];
    double vscale, sdiag, sdist, gun_pitch, aim_yaw, aim_pitch; int no_vr, gaze;
} Opts;

/* ------------------------------------------------------------------ scene worker: reads states and builds scenes off the render thread */
typedef struct {
    Opts *o; Nb1Roms *roms; Nb1TableSet *tset; Nb1Table *tab, *deftab;
    SDL_mutex *mu; GlScene back; int ready; unsigned back_frame; const char *back_table;
    volatile int quit, paused; volatile double zs; Source src;
} Worker;

static int worker_main(void *arg) {
    Worker *w = arg;
    while (!w->quit) {
        SDL_LockMutex(w->mu); int busy = w->ready; SDL_UnlockMutex(w->mu);
        if (busy || w->paused) { SDL_Delay(2); continue; }
        unsigned fr = 0; Nb1State *st = poll_state(&w->src, &fr);
        if (!st) { SDL_Delay(2); continue; }
        const Nb1Table *use = w->deftab; const char *name = NULL;
        if (w->tab) use = w->tab;
        else if (w->tset) { const Nb1Table *sel = nb1_tableset_select(w->tset, st, st, &name); if (sel) use = sel; else name = NULL; }
        w->back.zs = w->zs; w->back.no_rulers = 1;
        gl_scene_cpu(&w->back, st, w->roms, use);
        free(st);
        SDL_LockMutex(w->mu); w->ready = 1; w->back_frame = fr; w->back_table = name; SDL_UnlockMutex(w->mu);
    }
    return 0;
}

/* ------------------------------------------------------------------ OpenXR session */
typedef struct { XrSwapchain sc; int w, h; uint32_t n; XrSwapchainImageOpenGLKHR *img; GLuint fbo, depth; } Eye;
typedef struct {
    XrInstance inst; XrSystemId sys; XrSession sess; XrSpace local, aim_space[2]; int running, exit_requested, focused;
    XrActionSet set; XrAction a_aim, a_trigger, a_coin, a_start, a_pause, a_recenter, a_haptic; XrPath hand[2];
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
        make_action(xr, &xr->a_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1)) return -1;
    XrActionSuggestedBinding b[24]; int n = 0;
#define B(act, path) b[n].action = act; b[n++].binding = P(xr, path);
    B(xr->a_aim, "/user/hand/left/input/aim/pose") B(xr->a_aim, "/user/hand/right/input/aim/pose")
    B(xr->a_trigger, "/user/hand/left/input/trigger/value") B(xr->a_trigger, "/user/hand/right/input/trigger/value")
    B(xr->a_coin, "/user/hand/left/input/x/click") B(xr->a_coin, "/user/hand/right/input/a/click")
    B(xr->a_start, "/user/hand/left/input/y/click") B(xr->a_start, "/user/hand/right/input/b/click")
    B(xr->a_pause, "/user/hand/left/input/thumbstick/click") B(xr->a_recenter, "/user/hand/right/input/thumbstick/click")
    B(xr->a_haptic, "/user/hand/left/output/haptic") B(xr->a_haptic, "/user/hand/right/output/haptic")
    suggest(xr, "/interaction_profiles/oculus/touch_controller", b, n);
    n = 0;                                                                   /* Valve Index: A/B exist on both hands */
    B(xr->a_aim, "/user/hand/left/input/aim/pose") B(xr->a_aim, "/user/hand/right/input/aim/pose")
    B(xr->a_trigger, "/user/hand/left/input/trigger/value") B(xr->a_trigger, "/user/hand/right/input/trigger/value")
    B(xr->a_coin, "/user/hand/right/input/a/click") B(xr->a_start, "/user/hand/right/input/b/click")
    B(xr->a_pause, "/user/hand/left/input/thumbstick/click") B(xr->a_recenter, "/user/hand/right/input/thumbstick/click")
    B(xr->a_haptic, "/user/hand/left/output/haptic") B(xr->a_haptic, "/user/hand/right/output/haptic")
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
typedef struct { GLuint prog; GLint uMVP, uTex, uMode, uBox; GLuint vao, vbo, line_vao, line_vbo; } Gfx;

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
            glDrawArrays(GL_TRIANGLES, d->first, d->count);
        }
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL);
}
static void draw_lines(const Gfx *g, const M4 *mvp, const float *xyz, int nverts, float r, float gr, float b) {
    float m[16]; for (int i = 0; i < 16; i++) m[i] = (float)mvp->m[i];
    Vtx v[16]; for (int i = 0; i < nverts && i < 16; i++) v[i] = (Vtx){ xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2], 0, 0 };
    glDisable(GL_DEPTH_TEST); glUseProgram(g->prog); glUniformMatrix4fv(g->uMVP, 1, GL_FALSE, m); glUniform1i(g->uMode, 3); glUniform4f(g->uBox, r, gr, b, 1);
    glBindVertexArray(g->line_vao); glBindBuffer(GL_ARRAY_BUFFER, g->line_vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(Vtx) * nverts, v, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, nverts); glBindVertexArray(g->vao); glBindBuffer(GL_ARRAY_BUFFER, g->vbo); glEnable(GL_DEPTH_TEST);
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
int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter); setvbuf(stderr, NULL, _IONBF, 0);
#endif
    Opts o; memset(&o, 0, sizeof o); o.vscale = 1; o.sdiag = 29; o.sdist = 0.9;
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
        else if (!strcmp(a, "--gaze-aim")) o.gaze = 1;                                   /* aim with the head instead of a controller (testing, or no controllers) */
        else if (!strcmp(a, "--aim-offset") && v) { sscanf(v, "%lf,%lf", &o.aim_yaw, &o.aim_pitch); i++; }   /* yaw,pitch degrees added to the aim ray (testing) */
        else { fprintf(stderr, "usage: pbvr --roms DIR --ipc DIR [--tables scene_tables.json | --table T.json] [--screen-diag IN] [--screen-dist M] [--viewport-scale K] [--gun-pitch DEG] [--no-vr]\n"); return 2; }
    }
    if (!o.roms[0] || !o.ipc[0]) { fprintf(stderr, "pbvr: --roms and --ipc are required\n"); return 2; }
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
    gfx.uMVP = glGetUniformLocation(gfx.prog, "uMVP"); gfx.uTex = glGetUniformLocation(gfx.prog, "uTex"); gfx.uMode = glGetUniformLocation(gfx.prog, "uMode"); gfx.uBox = glGetUniformLocation(gfx.prog, "uBox");
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
    else if (o.tables[0]) { W.tset = nb1_tableset_load(o.tables); if (!W.tset) fprintf(stderr, "pbvr: cannot load tables %s\n", o.tables); }
    snprintf(W.src.ipc, sizeof W.src.ipc, "%s", o.ipc);
    double vw_m = o.sdiag * 0.0254 * 0.8 * o.vscale, zs = (NB1_W * o.sdist / vw_m) / NB1_FOCAL; W.zs = zs;
    SDL_Thread *thr = SDL_CreateThreadWithStackSize(worker_main, "scene", 64 << 20, &W);   /* the scene builder keeps big buffers on the stack; Windows threads default to 1 MB */

    GlScene front; memset(&front, 0, sizeof front); front.zs = zs; unsigned shown_frame = 0; const char *table_name = NULL;
    double origin[3] = { 0, 0, 0 }, yaw0 = 0; int need_recenter = 1;
    char aim_path[1100], aim_tmp[1100]; snprintf(aim_path, sizeof aim_path, "%s/aim.txt", o.ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s/aim.tmp", o.ipc);
    int seq = 0, trig = 0, coin = 0, start = 0, paused = 0, kcoin = 0, kstart = 0, active_hand = 1, prev_trig[2] = { 0, 0 }, prev_pause = 0, prev_rec = 0;
    double sx = 144, sy = 112, last_aim_write = 0, fps_t = now_s(); int fps_n = 0; int running = 1;
    int shot_wait = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { fprintf(stderr, "pbvr: window closed\n"); running = 0; }
            else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: fprintf(stderr, "pbvr: ESC pressed\n"); running = 0; break;
                case SDLK_c: kcoin = 1; break; case SDLK_v: kstart = 1; break;
                case SDLK_p: paused = !paused; break; case SDLK_r: need_recenter = 1; break;
                case SDLK_LEFTBRACKET: o.vscale /= 1.1; break; case SDLK_RIGHTBRACKET: o.vscale *= 1.1; break;
                default: break;
            } else if (ev.type == SDL_KEYUP) { if (ev.key.keysym.sym == SDLK_c) kcoin = 0; if (ev.key.keysym.sym == SDLK_v) kstart = 0; }
        }
        vw_m = o.sdiag * 0.0254 * 0.8 * o.vscale; zs = (NB1_W * o.sdist / vw_m) / NB1_FOCAL; W.zs = zs; W.paused = paused;

        /* take a finished scene from the worker */
        SDL_LockMutex(W.mu);
        if (W.ready) {
            gl_scene_free(&front); front = W.back; memset(&W.back, 0, sizeof W.back); shown_frame = W.back_frame; table_name = W.back_table; W.ready = 0;
            SDL_UnlockMutex(W.mu);
            front.no_rulers = 1; gl_scene_upload(&front);
            glBindBuffer(GL_ARRAY_BUFFER, gfx.vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(front.nverts ? front.nverts : 1) * sizeof(Vtx), front.verts, GL_DYNAMIC_DRAW);   /* the scene's geometry */
        } else SDL_UnlockMutex(W.mu);

        M4 view[2], proj[2]; int have_views = 0; XrPosef head = { { 0, 0, 0, 1 }, { 0, 0, 0 } }; int have_head = 0;
        double ray_o[3] = { 0, 0, 0 }, ray_d[3] = { 0, 0, -1 }; int have_ray = 0; double ctl_pos[3] = { 0, 0, 0 };
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        if (!o.no_vr) {
            xr_events(&xr); if (xr.exit_requested) { fprintf(stderr, "pbvr: the OpenXR session asked to exit\n"); break; }
            if (!xr.running) { SDL_Delay(20); continue; }
            XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO }; { XrResult wr = xrWaitFrame(xr.sess, &wi, &fs); if (XR_FAILED(wr)) { fprintf(stderr, "pbvr: xrWaitFrame failed (%d), stopping\n", (int)wr); break; } }
            XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO }; xrBeginFrame(xr.sess, &bi);
            xr.pred = fs.predictedDisplayTime;
            /* input */
            XrActiveActionSet as = { xr.set, XR_NULL_PATH }; XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO }; sy.countActiveActionSets = 1; sy.activeActionSets = &as; xrSyncActions(xr.sess, &sy);
            int trg[2], cn = 0, st = 0;
            for (int h = 0; h < 2; h++) {
                XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.subactionPath = xr.hand[h];
                XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT }; gi.action = xr.a_trigger; xrGetActionStateFloat(xr.sess, &gi, &f); trg[h] = f.isActive && f.currentState > 0.55f;
                XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN }; gi.action = xr.a_coin; xrGetActionStateBoolean(xr.sess, &gi, &b); if (b.isActive && b.currentState) cn = 1;
                gi.action = xr.a_start; b = (XrActionStateBoolean){ XR_TYPE_ACTION_STATE_BOOLEAN }; xrGetActionStateBoolean(xr.sess, &gi, &b); if (b.isActive && b.currentState) st = 1;
                if (trg[h] && !prev_trig[h]) active_hand = h;
            }
            if (trg[active_hand] == 0 && trg[1 - active_hand]) active_hand = 1 - active_hand;
            { XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
              gi.action = xr.a_pause; xrGetActionStateBoolean(xr.sess, &gi, &b); int p = b.isActive && b.currentState; if (p && !prev_pause) paused = !paused; prev_pause = p;
              gi.action = xr.a_recenter; b = (XrActionStateBoolean){ XR_TYPE_ACTION_STATE_BOOLEAN }; xrGetActionStateBoolean(xr.sess, &gi, &b); p = b.isActive && b.currentState; if (p && !prev_rec) need_recenter = 1; prev_rec = p; }
            if (trg[active_hand] && !trig) {                                                 /* a short buzz on each shot */
                XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = xr.a_haptic; hi.subactionPath = xr.hand[active_hand];
                XrHapticVibration vb = { XR_TYPE_HAPTIC_VIBRATION }; vb.duration = 25000000; vb.frequency = XR_FREQUENCY_UNSPECIFIED; vb.amplitude = 0.8f; xrApplyHapticFeedback(xr.sess, &hi, (XrHapticBaseHeader *)&vb);
            }
            prev_trig[0] = trg[0]; prev_trig[1] = trg[1]; trig = trg[active_hand]; coin = cn | kcoin; start = st | kstart;
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
                M4 to_scene = m4_mul(m4_scale_z(1.0 / zs), m4_rigid_inverse(m4_yaw_pos(yaw0, origin)));
                m4_point(to_scene, o_phys, ray_o); m4_dir(to_scene, d_phys, ray_d); have_ray = 1; memcpy(ctl_pos, o_phys, sizeof ctl_pos);
            }
            if (have_views) for (int e = 0; e < 2; e++) {
                M4 scene_to_phys = m4_mul(m4_yaw_pos(yaw0, origin), m4_scale_z(zs));
                view[e] = m4_mul(m4_rigid_inverse(m4_from_pose(xr.views[e].pose)), scene_to_phys); proj[e] = m4_proj(xr.views[e].fov, 0.05, 600.0);
            }
        }

        /* aim -> arcade pixel (the same snapping as pbdesk) */
        double hit_phys[3]; int have_hit = 0; static double dbg_rx, dbg_ry;
        if (have_ray && front.loaded) {
            Nb1AimSnap r;
            if (nb1_aim_snap(&front.scene, front.ids, ray_o, ray_d, 24.0, &r)) {
                dbg_rx = r.raw_sx; dbg_ry = r.raw_sy; sx = r.a.sx; sy = r.a.sy; M4 scene_to_phys = m4_mul(m4_yaw_pos(yaw0, origin), m4_scale_z(zs)); m4_point(scene_to_phys, r.a.hit, hit_phys); have_hit = 1;
            }
        }
        double t = now_s();
        if (t - last_aim_write > 1.0 / 120) {
            FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d %d\n", ++seq, sx, sy, trig, coin, start, paused); fclose(f); replace_file(aim_tmp, aim_path); }
            last_aim_write = t;
        }

        /* render */
        if (!o.no_vr) {
            XrCompositionLayerProjectionView pv[2]; XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            if (have_views && fs.shouldRender) {
                for (int e = 0; e < 2; e++) {
                    Eye *E = &xr.eye[e]; uint32_t idx = 0; XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; xrAcquireSwapchainImage(E->sc, &ai, &idx);
                    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(E->sc, &wi);
                    pBindFramebuffer(GL_FRAMEBUFFER, E->fbo); pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, E->img[idx].image, 0);
                    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, E->depth);
                    glViewport(0, 0, E->w, E->h); glClearColor(0.02f, 0.02f, 0.03f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    M4 mvp = m4_mul(proj[e], view[e]);
                    if (front.loaded) draw_scene(&gfx, &front, &mvp);
                    if (have_ray) {                                                   /* laser and a small cross where it lands (physical coordinates) */
                        M4 pv_ = m4_mul(proj[e], m4_rigid_inverse(m4_from_pose(xr.views[e].pose)));
                        double tip[3]; if (have_hit) memcpy(tip, hit_phys, sizeof tip); else { M4 sp = m4_mul(m4_yaw_pos(yaw0, origin), m4_scale_z(zs)); double far_s[3] = { ray_o[0] + ray_d[0] * 6, ray_o[1] + ray_d[1] * 6, ray_o[2] + ray_d[2] * 6 }; m4_point(sp, far_s, tip); }
                        float L[6] = { (float)ctl_pos[0], (float)ctl_pos[1], (float)ctl_pos[2], (float)tip[0], (float)tip[1], (float)tip[2] };
                        draw_lines(&gfx, &pv_, L, 2, trig ? 1.0f : 0.2f, trig ? 0.9f : 0.9f, trig ? 0.2f : 0.3f);
                        if (have_hit) { const float s = 0.02f; float X[8 * 3] = { (float)tip[0] - s, (float)tip[1], (float)tip[2], (float)tip[0] + s, (float)tip[1], (float)tip[2], (float)tip[0], (float)tip[1] - s, (float)tip[2], (float)tip[0], (float)tip[1] + s, (float)tip[2] };
                                         draw_lines(&gfx, &pv_, X, 4, 1.0f, 0.3f, 0.3f); }
                    }
                    if (e == 0 && getenv("PBVR_SHOT") && front.loaded && ++shot_wait == 30) {            /* PBVR_SHOT=file.ppm: save the left eye once a scene is showing (testing) */
                        uint8_t *px = malloc((size_t)E->w * E->h * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, E->w, E->h, GL_RGB, GL_UNSIGNED_BYTE, px);
                        FILE *sf = fopen(getenv("PBVR_SHOT"), "wb"); if (sf) { fprintf(sf, "P6\n%d %d\n255\n", E->w, E->h); for (int y = E->h - 1; y >= 0; y--) fwrite(px + (size_t)y * E->w * 3, 1, (size_t)E->w * 3, sf); fclose(sf); fprintf(stderr, "pbvr: saved %s\n", getenv("PBVR_SHOT")); }
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
    fprintf(stderr, "pbvr: leaving the main loop\n"); W.quit = 1; SDL_WaitThread(thr, NULL);
    if (!o.no_vr && xr.sess) { xrDestroySession(xr.sess); xrDestroyInstance(xr.inst); }
    return 0;
}
