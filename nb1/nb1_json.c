/* Minimal JSON reader for depth tables. */
#include "nb1_json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { const char *p; int err; } P;
static void ws(P *s) { while (isspace((unsigned char)*s->p)) s->p++; }
static Json *mk(JType t) { Json *j = calloc(1, sizeof *j); j->t = t; return j; }
static void add(Json *o, char *key, Json *v) {
    o->kid = realloc(o->kid, (o->n + 1) * sizeof *o->kid);
    if (o->t == J_OBJ) o->key = realloc(o->key, (o->n + 1) * sizeof *o->key);
    o->kid[o->n] = v; if (o->t == J_OBJ) o->key[o->n] = key; o->n++;
}
static char *str(P *s) {
    size_t cap = 16, n = 0; char *b = malloc(cap); s->p++;
    while (*s->p && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\') {
            c = *s->p++;
            switch (c) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                         case 'u': for (int i = 0; i < 4 && *s->p; i++) s->p++; c = '?'; break; default: break; }
        }
        if (n + 1 >= cap) b = realloc(b, cap *= 2);
        b[n++] = c;
    }
    if (*s->p != '"') s->err = 1; else s->p++;
    b[n] = 0; return b;
}
static Json *val(P *s) {
    ws(s);
    if (*s->p == '{') {
        Json *o = mk(J_OBJ); s->p++; ws(s);
        if (*s->p == '}') { s->p++; return o; }
        for (;;) {
            ws(s); if (*s->p != '"') { s->err = 1; return o; }
            char *k = str(s); ws(s); if (*s->p != ':') { s->err = 1; free(k); return o; } s->p++;
            add(o, k, val(s)); ws(s);
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == '}') { s->p++; return o; }
            s->err = 1; return o;
        }
    }
    if (*s->p == '[') {
        Json *a = mk(J_ARR); s->p++; ws(s);
        if (*s->p == ']') { s->p++; return a; }
        for (;;) {
            add(a, NULL, val(s)); ws(s);
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == ']') { s->p++; return a; }
            s->err = 1; return a;
        }
    }
    if (*s->p == '"') { Json *j = mk(J_STR); j->str = str(s); return j; }
    if (!strncmp(s->p, "true", 4)) { s->p += 4; Json *j = mk(J_BOOL); j->num = 1; return j; }
    if (!strncmp(s->p, "false", 5)) { s->p += 5; return mk(J_BOOL); }
    if (!strncmp(s->p, "null", 4)) { s->p += 4; return mk(J_NULL); }
    char *e; double d = strtod(s->p, &e);
    if (e == s->p) { s->err = 1; return mk(J_NULL); }
    s->p = e; Json *j = mk(J_NUM); j->num = d; return j;
}
Json *json_parse(const char *text) {
    P s = { text, 0 }; Json *j = val(&s); ws(&s);
    if (s.err || *s.p) { json_free(j); return NULL; }
    return j;
}
void json_free(Json *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) { json_free(j->kid[i]); if (j->key) free(j->key[i]); }
    free(j->kid); free(j->key); free(j->str); free(j);
}
const Json *json_get(const Json *o, const char *key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], key)) return o->kid[i];
    return NULL;
}
