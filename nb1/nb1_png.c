/* Minimal PNG reader (8-bit, non-interlaced: grey, grey+alpha, RGB, RGBA, palette +tRNS) using zlib. Returns RGBA. */
#include "nb1_internal.h"
#include <zlib.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int nb1_png_load(const char *path, int *w_out, int *h_out, uint8_t **rgba_out) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len); if (fread(b, 1, len, f) != (size_t)len) { fclose(f); free(b); return -1; } fclose(f);
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (len < 33 || memcmp(b, sig, 8)) { free(b); return -1; }
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, nplte = 0, ntrns = 0; uint8_t plte[256][3], trns[256];
    memset(trns, 255, sizeof trns);
    uint8_t *idat = NULL; size_t nidat = 0;
    for (long p = 8; p + 12 <= len;) {
        uint32_t cl = be32(b + p); const uint8_t *tag = b + p + 4, *d = b + p + 8;
        if (p + 12 + (long)cl > len) break;
        if (!memcmp(tag, "IHDR", 4)) { w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; interlace = d[12]; }
        else if (!memcmp(tag, "PLTE", 4)) { nplte = cl / 3 > 256 ? 256 : cl / 3; for (int i = 0; i < nplte; i++) memcpy(plte[i], d + 3 * i, 3); }
        else if (!memcmp(tag, "tRNS", 4)) { ntrns = cl > 256 ? 256 : cl; memcpy(trns, d, ntrns); }
        else if (!memcmp(tag, "IDAT", 4)) { idat = realloc(idat, nidat + cl); memcpy(idat + nidat, d, cl); nidat += cl; }
        else if (!memcmp(tag, "IEND", 4)) break;
        p += 12 + cl;
    }
    free(b);
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!w || !h || depth != 8 || !ch || interlace || !idat) { free(idat); return -1; }
    size_t stride = (size_t)w * ch, rawlen = (stride + 1) * h; uint8_t *raw = malloc(rawlen); uLongf outlen = rawlen;
    if (uncompress(raw, &outlen, idat, nidat) != Z_OK || outlen != rawlen) { free(idat); free(raw); return -1; }
    free(idat);
    uint8_t *img = calloc(stride * h, 1);
    for (int y = 0; y < h; y++) {                                 /* un-filter */
        int ft = raw[y * (stride + 1)]; const uint8_t *src = raw + y * (stride + 1) + 1; uint8_t *cur = img + y * stride, *prev = y ? cur - stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? cur[x - ch] : 0, up = prev ? prev[x] : 0, c = (prev && x >= (size_t)ch) ? prev[x - ch] : 0, v = src[x];
            switch (ft) {
            case 1: v += a; break; case 2: v += up; break; case 3: v += (a + up) / 2; break;
            case 4: { int pp = a + up - c, pa = abs(pp - a), pb = abs(pp - up), pc = abs(pp - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); break; }
            default: break;
            }
            cur[x] = (uint8_t)v;
        }
    }
    free(raw);
    uint8_t *out = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        const uint8_t *s = img + (size_t)i * ch; uint8_t *o = out + (size_t)i * 4;
        switch (ctype) {
        case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
        case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        case 3: if (s[0] < nplte) { o[0] = plte[s[0]][0]; o[1] = plte[s[0]][1]; o[2] = plte[s[0]][2]; } else o[0] = o[1] = o[2] = 0; o[3] = trns[s[0]]; break;
        case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
        default: memcpy(o, s, 4); break;
        }
    }
    (void)ntrns; free(img); *w_out = w; *h_out = h; *rgba_out = out; return 0;
}
