/* pbdesk: desktop viewer for the NB-1 depth scene (SDL2 + OpenGL 3.3 core).
 *
 *   pbdesk --roms DIR (--ipc DIR | --state FILE) [--table T.json | --tables scene_tables.json] [--res WxH] [--yaw N] [--pitch N] [--dist N] [--focus N] [--focal N]
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
#include "glscene.h"
/* ---------------------------------------------------------------- app */
typedef struct {
    char roms[512], ipc[512], statefile[512], table[512], tables[512], shot[512];
    int w, h, mode, hide_hud, use_table, paused, shot_after, show_help, no_text, shot_game_frame, frames_limit, mouse_x, mouse_y, mouse_set, trigger_set, ruler;
    double vscale, sdiag, sdist;
    double yaw, pitch, dist, focus, focal;
} Opts;

static void die_usage(void) {
    fprintf(stderr, "usage: pbdesk --roms DIR (--ipc DIR | --state FILE) [--table T.json | --tables scene_tables.json] [--res WxH] [--yaw N] [--pitch N] [--dist N]\n"
                    "              [--focus N] [--focal N] [--hide-hud] [--no-text] [--no-ruler | --ruler-all] [--viewport-scale K] [--screen-diag IN] [--screen-dist M] [--shot out.ppm [--shot-after N | --shot-game-frame N]]\n");
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
    Opts o; memset(&o, 0, sizeof o); o.w = 960; o.h = 600; o.mode = 1; o.use_table = 1; o.ruler = 1; o.vscale = 1.0; o.sdiag = 29.0; o.sdist = 0.9; o.pitch = 14; o.dist = 15; o.focus = 9; o.focal = 620; o.shot_after = 8;
    int yaw_given = 0; o.yaw = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--roms") && v) { snprintf(o.roms, sizeof o.roms, "%s", v); i++; }
        else if (!strcmp(a, "--ipc") && v) { snprintf(o.ipc, sizeof o.ipc, "%s", v); i++; }
        else if (!strcmp(a, "--state") && v) { snprintf(o.statefile, sizeof o.statefile, "%s", v); i++; }
        else if (!strcmp(a, "--table") && v) { snprintf(o.table, sizeof o.table, "%s", v); i++; }
        else if (!strcmp(a, "--tables") && v) { snprintf(o.tables, sizeof o.tables, "%s", v); i++; }          /* scene_tables.json: pick a table per scene */
        else if (!strcmp(a, "--res") && v) { if (sscanf(v, "%dx%d", &o.w, &o.h) != 2) die_usage(); i++; }
        else if (!strcmp(a, "--yaw") && v) { o.yaw = atof(v); yaw_given = 1; o.mode = 2; i++; }
        else if (!strcmp(a, "--pitch") && v) { o.pitch = atof(v); i++; }
        else if (!strcmp(a, "--dist") && v) { o.dist = atof(v); i++; }
        else if (!strcmp(a, "--focus") && v) { o.focus = atof(v); i++; }
        else if (!strcmp(a, "--focal") && v) { o.focal = atof(v); i++; }
        else if (!strcmp(a, "--hide-hud")) o.hide_hud = 1;
        else if (!strcmp(a, "--no-text")) o.no_text = 1;
        else if (!strcmp(a, "--no-ruler")) o.ruler = 0;
        else if (!strcmp(a, "--viewport-scale") && i + 1 < argc) o.vscale = atof(argv[++i]);
        else if (!strcmp(a, "--screen-diag") && i + 1 < argc) o.sdiag = atof(argv[++i]);
        else if (!strcmp(a, "--screen-dist") && i + 1 < argc) o.sdist = atof(argv[++i]);
        else if (!strcmp(a, "--ruler-all")) o.ruler = 2;
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
    Nb1TableSet *tset = (o.tables[0] && !o.table[0]) ? nb1_tableset_load(o.tables) : NULL;     /* an explicit --table wins */
    if (o.tables[0] && !o.table[0] && !tset) fprintf(stderr, "pbdesk: cannot read %s (using the default table)\n", o.tables);
    const char *table_name = NULL;

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
    GLuint vao3, vbo3, vao4, vbo4;                                        /* metre rulers (lines) and their labels (billboards) */
    glGenVertexArrays(1, &vao3); glBindVertexArray(vao3); glGenBuffers(1, &vbo3); glBindBuffer(GL_ARRAY_BUFFER, vbo3);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)(3 * sizeof(float)));
    glGenVertexArrays(1, &vao4); glBindVertexArray(vao4); glGenBuffers(1, &vbo4); glBindBuffer(GL_ARRAY_BUFFER, vbo4);
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
                case SDLK_LEFTBRACKET: o.vscale /= 1.1; dirty = 1; break;                  /* smaller / bigger viewport */
                case SDLK_RIGHTBRACKET: o.vscale *= 1.1; dirty = 1; break;
                case SDLK_m: o.ruler = (o.ruler + 1) % 3; break;                           /* off / main layers / every depth group */
                case SDLK_t: o.use_table = !o.use_table; dirty = 1; break;
                case SDLK_r: if (tset) { Nb1TableSet *nt = nb1_tableset_load(o.tables); if (nt) { nb1_tableset_free(tset); tset = nt; dirty = 1; } else fprintf(stderr, "pbdesk: tables reload failed\n"); }
                          if (o.table[0]) { Nb1Table *nt = nb1_table_load(o.table); if (nt) { nb1_table_free(tab); tab = nt; dirty = 1; } else fprintf(stderr, "pbdesk: table reload failed\n"); } break;
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
        /* physical viewport: width W at distance D shows the 288 px picture, so one px subtends W/(288 D) and the arcade eye's f = 346 becomes f_eff = 288 D / W */
        const double vw_m = o.sdiag * 0.0254 * 0.8 * o.vscale, feff = NB1_W * o.sdist / vw_m, zs = feff / NB1_FOCAL, fov = 2 * atan(vw_m / (2 * o.sdist)) * 180 / M_PI;
        if (gs.zs != zs) { gs.zs = zs; dirty = 1; }
        if (dirty && cur) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            const Nb1Table *use = deftab; table_name = NULL;
            if (o.use_table && tab) use = tab;
            else if (o.use_table && tset) { const Nb1Table *sel = nb1_tableset_select(tset, cur, cur, &table_name); if (sel) use = sel; else table_name = NULL; }
            gl_scene_build(&gs, cur, &roms, use);
            build_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)gs.nverts * sizeof(Vtx), gs.verts, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, vbo3); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gs.nrverts ? gs.nrverts : 1) * sizeof(Vtx), gs.rverts, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
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
            const double zi = o.mode == 1 ? 1.0 : zs, Ei[3] = { cam.E[0], cam.E[1], cam.E[2] / zi };      /* physical -> authored coordinates */
            d[2] /= zi;
            Nb1AimSnap r;
            if (!nb1_aim_snap(&gs.scene, gs.ids, Ei, d, 24.0, &r))
                snprintf(aim_txt, sizeof aim_txt, "aim: no hit, keeping last pixel (%5.1f,%5.1f)   [scene frame %u]", sx, sy, shown_frame);
            else {
                const Nb1Item *it = &gs.scene.items[r.a.item];
                const char *kn = it->kind == NB1_TILEMAP ? "tilemap" : it->kind == NB1_SPRITE ? "sprite" : "mask";
                if (r.hidden) {
                    sx = r.a.sx; sy = r.a.sy;
                    const Nb1Item *cv = r.cover >= 0 ? &gs.scene.items[r.cover] : NULL;
                    snprintf(aim_txt, sizeof aim_txt, "aim HIDDEN: %s%d z=%.1f is covered here; shot lands on %s%d (%5.1f,%5.1f)", kn, it->id, it->z * zi,
                             cv ? (cv->kind == NB1_TILEMAP ? "tilemap" : "sprite") : "nothing", cv ? cv->id : 0, sx, sy);
                } else {
                    sx = r.a.sx; sy = r.a.sy;
                    snprintf(aim_txt, sizeof aim_txt, "aim px (%5.1f,%5.1f) hits %s%d%s%s z=%.1f%s   [scene frame %u]", sx, sy, kn, it->id, it->has_part ? ":" : "", it->has_part ? it->part : "", it->z * zi,
                             r.snapped ? "  SNAPPED" : "", shown_frame);
                }
            }
        }
        if (o.ipc[0]) {                                           /* publish the aim for the game */
            FILE *f = fopen(aim_tmp, "w");
            if (f) { fprintf(f, "%d %.2f %.2f %d %d %d %d\n", ++seq, sx, sy, trig, coin, start, o.paused); fclose(f); replace_file(aim_tmp, aim_path); }
        }

        /* draw */
        glViewport(0, 0, ww, wh); glClearColor(0.08f, 0.08f, 0.09f, 1); glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (gs.loaded) {
            glViewport(vx, vy, vw, vh); glEnable(GL_SCISSOR_TEST); glScissor(vx, vy, vw, vh);
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            float mvp[16]; make_mvp(&cam, vw, vh, o.mode == 1 ? 1.0 : zs, mvp);
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
            const int rlimit = o.ruler == 1 ? 10 : 24;
            if (o.mode == 2 && o.ruler && gs.nrulers) {                            /* metre rulers: always on top so their scale can be read */
                glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glUseProgram(prog); glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp); glUniform1i(uTex, 0);
                glBindVertexArray(vao3); glUniform1i(uMode, 3);
                for (int ri = 0; ri < gs.nrulers; ri++) {
                    const Ruler *r = &gs.rulers[ri]; if ((o.hide_hud && r->hud) || ri >= rlimit) continue;
                    glUniform4f(uBox, r->col[0], r->col[1], r->col[2], 1.0f); glDrawArrays(GL_LINES, r->first, r->count);
                }
                glBindVertexArray(vao4); glUniform1i(uMode, 4);
                for (int li = 0; li < gs.nlabels; li++) {
                    const Label *l = &gs.labels[li]; if ((o.hide_hud && l->hud) || l->ruler >= rlimit) continue;
                    const double lz = l->pos[2] * zs; double dx = l->pos[0] - cam.E[0], dy = l->pos[1] - cam.E[1], dz = lz - cam.E[2], dist = sqrt(dx * dx + dy * dy + dz * dz);
                    double hh = 17.0 * dist / cam.f, ww_ = hh * l->tw / l->th;                     /* about 17 px tall on screen */
                    double x0 = l->right_align ? -ww_ : 0.0, x1 = x0 + ww_, y0 = l->right_align ? -hh / 2 : 0.0, y1 = y0 + hh; Vtx q[6]; double cx_[4] = { x0, x1, x1, x0 }, cy_[4] = { y1, y1, y0, y0 };
                    static const float uvq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }; Vtx c4[4];
                    for (int k = 0; k < 4; k++) { c4[k] = (Vtx){ (float)(l->pos[0] + cam.right[0] * cx_[k] + cam.up[0] * cy_[k]), (float)(l->pos[1] + cam.right[1] * cx_[k] + cam.up[1] * cy_[k]),
                                                               (float)(lz + cam.right[2] * cx_[k] + cam.up[2] * cy_[k]), uvq[k][0], uvq[k][1] }; }
                    q[0] = c4[0]; q[1] = c4[1]; q[2] = c4[2]; q[3] = c4[0]; q[4] = c4[2]; q[5] = c4[3];
                    glBindBuffer(GL_ARRAY_BUFFER, vbo4); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
                    glBindTexture(GL_TEXTURE_2D, l->tex); glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            }
            glDisable(GL_BLEND); glDepthMask(GL_TRUE); glDisable(GL_SCISSOR_TEST);
        }
        /* status text */
        if (!o.no_text) {
            char l1[256], l2[640]; const char *tn = !o.use_table ? "off (auto depth)" : (o.table[0] ? strrchr(o.table, '/') ? strrchr(o.table, '/') + 1 : o.table : (table_name ? table_name : "default"));
            snprintf(l1, sizeof l1, "%s  yaw %.1f pitch %.1f dist %.1f focus %.1f   game frame %u  build %.0f ms%s", o.mode == 1 ? "ARCADE" : "ORBIT", o.yaw, o.pitch, o.dist, o.focus, shown_frame, build_ms, o.paused ? "  PAUSED" : "");
            snprintf(l2, sizeof l2, "table: %s   viewport %.2f m at %.2f m (%.1f deg, x%.2f, [ ] to change)  depth x%.2f   F1 help", tn, vw_m, o.sdist, fov, o.vscale, zs);
            const char *help[] = { "1 arcade view  2 orbit  A/D yaw  W/S pitch  Q/E distance  Z/X focus  right-drag or Alt+drag orbit  wheel distance",
                                   "H hide HUD  M metre rulers off/main/all (orbit view)  T table on/off  R reload table  P pause  F5 screenshot  ESC quit   C coin  V start  left mouse = trigger" };
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
    gl_scene_free(&gs); free(cur); nb1_tableset_free(tset); nb1_table_free(tab); nb1_table_free(deftab); nb1_roms_free(&roms);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
