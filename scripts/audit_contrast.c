/* Syntax token contrast audit - P1 #4 of docs/UI_PLAN.md.
 *
 * Builds every appearance style preset's theme in-process (cui__theme_build_ex,
 * the same code the app runs), derives the code editor's background exactly as
 * cui_editor.c does, then calls the real cui__syntax_colors_ex so the audit
 * can never drift from what ships. Measures WCAG contrast of each token color
 * against the surface, plus OKLab distance between token colors so
 * near-duplicates show up (number and const are identical by design and are
 * excluded from that check).
 *
 * Build (from repo root, MSVC env):
 *   cl /nologo /W4 /O2 /MD scripts\audit_contrast.c /I vendor\CUI\include
 *      /I vendor\CUI\src /Fe:build-msvc\audit_contrast.exe /link
 *      build-msvc\vendor\CUI\cui.lib user32.lib gdi32.lib ...
 */
#include <stdio.h>
#include <math.h>
#include "cui_internal.h"

typedef struct { const char *name; cui_theme_mode mode; cui_accent accent; float hue, chroma; } style;
/* mirrors np_styles in src/rapideditor.c */
static const style styles[] = {
    { "aurora-blue",  CUI_THEME_LIGHT, CUI_ACCENT_BLUE,   255.f, 0.011f },
    { "silver-gray",  CUI_THEME_LIGHT, CUI_ACCENT_BLUE,   255.f, 0.0f   },
    { "warm-sand",    CUI_THEME_LIGHT, CUI_ACCENT_AMBER,   70.f, 0.014f },
    { "moss-green",   CUI_THEME_LIGHT, CUI_ACCENT_GREEN,  155.f, 0.013f },
    { "violet",       CUI_THEME_LIGHT, CUI_ACCENT_VIOLET, 290.f, 0.013f },
    { "deep-sea",     CUI_THEME_DARK,  CUI_ACCENT_BLUE,   255.f, 0.012f },
    { "obsidian",     CUI_THEME_DARK,  CUI_ACCENT_NEUTRAL,255.f, 0.0f   },
    { "midnight-p",   CUI_THEME_DARK,  CUI_ACCENT_VIOLET, 290.f, 0.014f },
    { "charcoal",     CUI_THEME_DARK,  CUI_ACCENT_AMBER,   60.f, 0.013f },
};
#define NS ((int)(sizeof styles / sizeof styles[0]))

static const char *tk_name[] = { "text", "keyword", "type", "string", "number",
                                 "comment", "func", "preproc", "punct", "const" };
enum { TK_TEXT, TK_KEYWORD, TK_TYPE, TK_STRING, TK_NUMBER, TK_COMMENT,
       TK_FUNC, TK_PREPROC, TK_PUNCT, TK_CONST, TK_N };

static double srgb_lin(unsigned v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double rel_lum(cui_color c) {
    return 0.2126 * srgb_lin(c.r) + 0.7152 * srgb_lin(c.g) + 0.0722 * srgb_lin(c.b);
}
static double contrast(cui_color a, cui_color b) {
    double la = rel_lum(a), lb = rel_lum(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}
/* sRGB -> OKLab, for token distinctness */
static void oklab(cui_color c, double *L, double *A, double *B) {
    double r = srgb_lin(c.r), g = srgb_lin(c.g), b = srgb_lin(c.b);
    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    double l_ = cbrt(l), m_ = cbrt(m), s_ = cbrt(s);
    *L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    *A = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    *B = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
}
static double oklab_dist(cui_color a, cui_color b) {
    double L1, A1, B1, L2, A2, B2;
    oklab(a, &L1, &A1, &B1); oklab(b, &L2, &A2, &B2);
    return sqrt((L1-L2)*(L1-L2) + (A1-A2)*(A1-A2) + (B1-B2)*(B1-B2));
}

int main(void) {
    int fails = 0, close_pairs = 0;
    for (int s = 0; s < NS; s++) {
        cui_theme t;
        cui__theme_build_ex(&t, styles[s].mode, styles[s].accent, styles[s].hue, styles[s].chroma);
        /* the code editor's surface, exactly as cui_editor.c paints it */
        cui_color bg = styles[s].mode == CUI_THEME_DARK
            ? cui_mix(t.colors[CUI_TOKEN_SURFACE], t.colors[CUI_TOKEN_BACKGROUND], 0.4f)
            : t.colors[CUI_TOKEN_SURFACE_MUTED];
        bool dark = styles[s].mode == CUI_THEME_DARK;
        cui_color c[TK_N];
        cui__syntax_colors_ex(&t, dark, c);

        printf("\n== %s (%s, bg #%02x%02x%02x)\n", styles[s].name,
               dark ? "dark" : "light", bg.r, bg.g, bg.b);
        for (int k = 0; k < TK_N; k++) {
            double cr = contrast(c[k], bg);
            if (cr < 4.5) fails++;
            printf("  %-8s #%02x%02x%02x  %.2f:1%s\n", tk_name[k], c[k].r, c[k].g, c[k].b,
                   cr, cr < 4.5 ? "  << FAIL <4.5" : "");
        }
        /* distinctness: the classic confusions must stay apart */
        static const int pairs[][2] = {
            {TK_KEYWORD, TK_TYPE}, {TK_KEYWORD, TK_FUNC}, {TK_STRING, TK_NUMBER},
            {TK_STRING, TK_CONST}, {TK_COMMENT, TK_PUNCT}, {TK_TYPE, TK_FUNC},
            {TK_KEYWORD, TK_STRING},
        };
        for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
            double d = oklab_dist(c[pairs[i][0]], c[pairs[i][1]]);
            if (d < 0.07) {
                close_pairs++;
                printf("  !! close: %s vs %s  dE=%.3f\n", tk_name[pairs[i][0]],
                       tk_name[pairs[i][1]], d);
            }
        }
    }
    printf("\n%d contrast failures, %d too-close pairs\n", fails, close_pairs);
    return fails + close_pairs;
}
