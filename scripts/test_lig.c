/* Ligature regression: the internal OpenType shaper must agree with
 * HarfBuzz on the glyph ids and total advance for programming ligature
 * sequences in both bundled ligature faces (JetBrains Mono, Cascadia Code).
 * Reference values produced with uharfbuzz 0.57 (HarfBuzz 12), fonts from
 * assets/fonts as of 2026-11.  Editor rendering wiring is documented in
 * docs/NOTES.md (P4 #17): the editor draws lines in token/viewport slices,
 * which breaks the context of sequences split across slices - this test
 * pins the shaper itself so future slice-based drawing can rely on it. */
#include "cui_shape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *text; const int *ids; int n; int adv; } lig_case;

static int test(cui_shape_font *f, const char *fontname, const lig_case *c) {
    int n = (int)strlen(c->text);
    cui_shape_glyph g[64];
    memset(g, 0, sizeof g);
    for (int i = 0; i < n; i++) g[i].codepoint = (unsigned char)c->text[i];
    int out = cui__shape(f, g, n, 64, 0x6c61746e /*latn*/, 0, 0);
    if (out != c->n) {
        printf("FAIL %-16s %-4s: out=%d expected %d glyphs\n", fontname, c->text, out, c->n);
        return 1;
    }
    int adv = 0, bad = 0;
    for (int i = 0; i < out; i++) {
        adv += g[i].advance;
        if (g[i].id != c->ids[i]) bad = 1;
    }
    if (bad || adv != c->adv) {
        printf("FAIL %-16s %-4s: ids/adv", fontname, c->text);
        for (int i = 0; i < out; i++) printf(" %d", g[i].id);
        printf(" (adv %d) expected", adv);
        for (int i = 0; i < c->n; i++) printf(" %d", c->ids[i]);
        printf(" (adv %d)\n", c->adv);
        return 1;
    }
    printf("ok   %-16s %-4s -> %d glyph(s), adv %d\n", fontname, c->text, out, adv);
    return 0;
}

int main(int argc, char **argv) {
    static const int jb_arrow[] = {1751, 841}, jb_arrow2[] = {1751, 1006},
                     jb_neq[] = {1751, 872}, jb_eq3[] = {1751, 1751, 1004},
                     jb_amp[] = {1751, 980}, jb_abc[] = {198, 223, 224};
    static const int cas_arrow[] = {842, 1437}, cas_arrow2[] = {1425, 1431},
                     cas_neq[] = {812, 1774}, cas_eq3[] = {1378, 1774, 1774},
                     cas_amp[] = {1365, 1774}, cas_abc[] = {222, 249, 250};
    static const lig_case jb[] = {
        { "->",  jb_arrow, 2, 1200 }, { "=>",  jb_arrow2, 2, 1200 },
        { "!=",  jb_neq,   2, 1200 }, { "===", jb_eq3,    3, 1800 },
        { "&&",  jb_amp,   2, 1200 }, { "abc", jb_abc,   3, 1800 },
    };
    static const lig_case cas[] = {
        { "->",  cas_arrow, 2, 2400 }, { "=>",  cas_arrow2, 2, 2400 },
        { "!=",  cas_neq,   2, 2400 }, { "===", cas_eq3,    3, 3600 },
        { "&&",  cas_amp,   2, 2400 }, { "abc", cas_abc,   3, 3600 },
    };

    int fail = 0;
    const char *files[] = { "assets/fonts/JetBrainsMono-Regular.ttf",
                            "assets/fonts/CascadiaCode-Regular.ttf" };
    for (int fi = 0; fi < 2; fi++) {
        FILE *fp = fopen(files[fi], "rb");
        if (!fp) { printf("FAIL cannot open %s\n", files[fi]); fail++; continue; }
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        unsigned char *data = malloc((size_t)size);
        if (fread(data, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); fail++; continue; }
        fclose(fp);
        cui_shape_font f; memset(&f, 0, sizeof f);
        if (cui__shape_font_init(&f, data, (size_t)size, 0) != 1) {
            printf("FAIL %s: shape_font_init\n", files[fi]); free(data); fail++; continue;
        }
        if (fi == 0)
            for (unsigned k = 0; k < sizeof jb / sizeof *jb; k++) fail += test(&f, "JetBrainsMono", &jb[k]);
        else
            for (unsigned k = 0; k < sizeof cas / sizeof *cas; k++) fail += test(&f, "CascadiaCode", &cas[k]);
        free(data);
    }
    return fail ? 1 : 0;
}
