/* Internal trace: include cui_shape.c so we can call its static helpers and
 * step through the GSUB path manually for the '->' sequence, printing where
 * the chain breaks. */
#include "cui_shape.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "assets/fonts/CascadiaCode-Regular.ttf";
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return 2; }
    fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) return 2; fclose(fp);

    cui_shape_font f; memset(&f, 0, sizeof f);
    if (cui__shape_font_init(&f, data, (size_t)size, 0) != 1) { printf("init failed\n"); return 1; }

    int g_dash = cui__shape_map(&f, '-'), g_gt = cui__shape_map(&f, '>');
    printf("cmap: '-' -> gid %d, '>' -> gid %d\n", g_dash, g_gt);

    off lang = language(&f, f.gsub, 0x6c61746e);   /* latn */
    printf("langsys=%u\n", lang);
    if (!lang) { printf("no langsys\n"); return 1; }
    off features = rel(&f, f.gsub, u16(&f, f.gsub+6));
    int calt_index = -1;
    for (unsigned k = 0; k < u16(&f, lang+4); k++) {
        unsigned index = u16(&f, lang+6+2*k);
        off r = features+2+6*index;
        if (u32(&f, r) == 0x63616c74) { calt_index = (int)index; break; }
    }
    printf("calt feature index=%d\n", calt_index);
    if (calt_index < 0) { printf("no calt in langsys\n"); return 1; }
    off fr = features+2+6*(unsigned)calt_index;
    off fp2 = rel(&f, features, u16(&f, fr+4));
    unsigned nlk = u16(&f, fp2+2);
    printf("calt lookups=%u\n", nlk);

    cui_shape_glyph g[8];
    memset(g, 0, sizeof g);
    g[0].codepoint = '-'; g[1].codepoint = '>';
    shape s; memset(&s, 0, sizeof s);
    s.f = &f; s.g = g; s.n = 2; s.cap = 8; s.rtl = 0; s.position = 0;
    s.layout = f.gsub; s.list = rel(&f, f.gsub, u16(&f, f.gsub+8));
    for (int i = 0; i < 2; i++) {
        g[i].id = cui__shape_map(&f, g[i].codepoint);
        g[i].x = g[i].y = 0; g[i].form = -1; g[i].ligature = g[i].component = 0;
    }
    /* advance from hmtx like cui__shape does */
    for (int i = 0; i < 2; i++) {
        int glyph = g[i].id;
        g[i].advance = u16(&f, f.hmtx+4*(glyph < (int)f.metrics ? glyph : (int)f.metrics-1));
    }
    printf("gids %d adv %d | %d adv %d\n", g[0].id, g[0].advance, g[1].id, g[1].advance);

    for (unsigned l = 0; l < nlk; l++) {
        unsigned lookup = u16(&f, fp2+4+2*l);
        off lp = rel(&f, s.list, u16(&f, s.list+2+2*lookup));
        unsigned type = u16(&f, lp), flags = u16(&f, lp+2), nsub = u16(&f, lp+4);
        /* check subtable[0] coverage against g[0].id for type 6 fmt3 */
        int cov0 = -1, n_in = -1;
        for (unsigned j = 0; j < nsub && cov0 < 0; j++) {
            off st = rel(&f, lp, u16(&f, lp+6+2*j));
            if (type != 6) continue;
            unsigned fmt = u16(&f, st);
            if (fmt != 3) continue;
            off q = st+2;
            unsigned nbt = u16(&f, q); q += 2+2*nbt;
            unsigned nin = u16(&f, q); q += 2;
            cov0 = coverage(&f, rel(&f, st, u16(&f, q)), g[0].id);
            n_in = (int)nin;
        }
        int applied = apply(&s, lookup, 0, 0);
        int subj = -1, bt = -1, la = -1, rec = -1, rli = -1;
        for (unsigned j = 0; j < nsub && subj < 0; j++) {
            off st = rel(&f, lp, u16(&f, lp+6+2*j));
            if (u16(&f, st) != 3) continue;
            off q = st+2;
            int nbt = u16(&f, q); q += 2+2*nbt;
            int nin = u16(&f, q); q += 2;
            if (coverage(&f, rel(&f, st, u16(&f, q)), g[0].id) < 0) continue;
            subj = (int)j; bt = nbt; la = -1; rec = -1;
            q += 2;
            for (int k = 1; k < nin; k++) q += 2;   /* remaining input covs */
            la = u16(&f, q); q += 2+2*la;
            rec = u16(&f, q);
            if (rec > 0) rli = (int)u16(&f, q+2+4*0+2);
        }
        printf("lookup %3u type %u flags %u sub %u: cov0=%d apply=%d  hit-sub %d bt %d la %d records %d sublookup %d\n",
               lookup, type, flags, nsub, cov0, applied, subj, bt, la, rec, rli);
    }
    printf("final glyph count=%d\n", s.n);
    return 0;
}
