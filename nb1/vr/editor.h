/* Pause-mode scene editor for pbvr (included by pbvr.c): pick a layer or a sprite type, scale its depth, jump to it, save the changes.
 * "Scaling" a layer moves it along the arcade eye's rays (depth times k), so the arcade picture does not change; only its real size / distance does.
 * The edits are kept per scene (the captured scene whose background art is nearest) and are applied by the library as a table's "scales". */
#pragma once


static int spec_same(const Nb1ScaleSpec *a, const Nb1ScaleSpec *b) {
    return a->kind == b->kind && a->layer == b->layer && a->pri == b->pri && a->has_code == b->has_code && (!a->has_code || (a->code_lo == b->code_lo && a->code_hi == b->code_hi));
}
/* scope 0: the whole layer (a tilemap layer, or every sprite of that priority); scope 1: every sprite with that tile code */
static int spec_from_item(const SelItem *it, int scope, Nb1ScaleSpec *s) {
    memset(s, 0, sizeof *s); s->kind = -1; s->layer = -1; s->pri = -1; s->k = 1;
    if (it->kind == NB1_TILEMAP) { s->kind = NB1_TILEMAP; s->layer = it->id; return 1; }
    if (it->kind != NB1_SPRITE) return 0;
    s->kind = NB1_SPRITE;
    if (scope == 1 && it->has_key) { s->has_code = 1; s->code_lo = s->code_hi = it->key; } else s->pri = it->pri;
    return 1;
}
static int spec_matches(const Nb1ScaleSpec *s, const Nb1Item *it) {
    if (it->hud || it->decal || it->kind == NB1_MASK) return 0;
    if (s->kind >= 0 && it->kind != s->kind) return 0;
    if (s->layer >= 0 && !(it->kind == NB1_TILEMAP && it->id == s->layer)) return 0;
    if (s->pri >= 0 && it->pri != s->pri) return 0;
    if (s->has_code && !(it->has_key && it->key >= s->code_lo && it->key <= s->code_hi)) return 0;
    return 1;
}
static void spec_desc(const Nb1ScaleSpec *s, char *out, size_t n) {
    if (s->kind == NB1_TILEMAP) snprintf(out, n, "tilemap layer %d", s->layer);
    else if (s->has_code) snprintf(out, n, "sprite code %d", s->code_lo);
    else snprintf(out, n, "sprites of priority %d", s->pri);
}
static void spec_json(const Nb1ScaleSpec *s, char *out, size_t n) {
    if (s->kind == NB1_TILEMAP) snprintf(out, n, "{\"kind\": \"tilemap\", \"layer\": [%d]}", s->layer);
    else if (s->has_code) snprintf(out, n, "{\"kind\": \"sprite\", \"code\": [%d, %d]}", s->code_lo, s->code_hi);
    else snprintf(out, n, "{\"kind\": \"sprite\", \"pri\": [%d]}", s->pri);
}
static int cmp_dbl(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
/* median authored depth and item count of a group in a built scene */
static int group_z(const GlScene *g, const Nb1ScaleSpec *s, double *zmed) {
    double z[1200]; int n = 0;
    for (int i = 0; i < g->scene.n && n < 1200; i++) if (spec_matches(s, &g->scene.items[i])) z[n++] = g->scene.items[i].z;
    if (!n) return 0;
    qsort(z, (size_t)n, sizeof *z, cmp_dbl); *zmed = z[n / 2]; return n;
}

/* the editor's shared list of edits (the scene worker reads it when it builds) */
static OvEntry *ov_find(OvEntry *ov, int nov, int scene, const Nb1ScaleSpec *s) {
    for (int i = 0; i < nov; i++) if (ov[i].scene == scene && spec_same(&ov[i].s, s)) return &ov[i];
    return NULL;
}

/* ---- text panel (head-up lines drawn in the world, in front of the arcade eye's frame) */
#define PANEL_LINES 12
typedef struct { char text[PANEL_LINES][200]; GLuint tex[PANEL_LINES]; int tw[PANEL_LINES], th[PANEL_LINES]; } Panel;
static void panel_set(Panel *p, int i, const char *s) {
    if (!strcmp(p->text[i], s) && p->tex[i]) return;
    if (p->tex[i]) glDeleteTextures(1, &p->tex[i]);
    snprintf(p->text[i], sizeof p->text[i], "%s", s); p->tex[i] = text_tex(s[0] ? s : " ", &p->tw[i], &p->th[i]);
}
/* frame = yaw/position of the arcade eye; the panel hangs below the picture, facing the eye; px = metres per text pixel */
static void panel_draw(const Gfx *g, const Panel *p, const M4 *vp, M4 frame, double dist, double y_top, double px, int left_align) {
    double maxw = 0; for (int i = 0; i < PANEL_LINES; i++) if (p->tex[i] && p->text[i][0] && p->tw[i] * px > maxw) maxw = p->tw[i] * px;
    float m[16]; for (int i = 0; i < 16; i++) m[i] = (float)vp->m[i];
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glUseProgram(g->prog); glUniformMatrix4fv(g->uMVP, 1, GL_FALSE, m); glUniform1i(g->uTex, 0); glUniform1i(g->uMode, 4);
    glBindVertexArray(g->line_vao); glBindBuffer(GL_ARRAY_BUFFER, g->line_vbo);
    double y = y_top;
    for (int i = 0; i < PANEL_LINES; i++) {
        if (!p->tex[i] || !p->text[i][0]) continue;
        double w = p->tw[i] * px, h = p->th[i] * px, x0 = left_align ? -maxw / 2 : -w / 2, x1 = x0 + w, y1 = y, y0 = y - h; y = y0 - 0.004;
        double cx[4] = { x0, x1, x1, x0 }, cy[4] = { y1, y1, y0, y0 }; static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }; Vtx c[4], q[6];
        for (int k = 0; k < 4; k++) { double v[3] = { cx[k], cy[k], -dist }, o[3]; m4_point(frame, v, o); c[k] = (Vtx){ (float)o[0], (float)o[1], (float)o[2], uv[k][0], uv[k][1] }; }
        q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = c[0]; q[4] = c[2]; q[5] = c[3];
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW); glBindTexture(GL_TEXTURE_2D, p->tex[i]); glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(g->vao); glBindBuffer(GL_ARRAY_BUFFER, g->vbo); glEnable(GL_DEPTH_TEST);
}
