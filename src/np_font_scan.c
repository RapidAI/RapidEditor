#include "np_font_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

/* Simple dynamic list of owned strings. */
typedef struct { char **v; int n, cap; } slist;

static void sl_free(slist *s) {
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v); s->v = NULL; s->n = s->cap = 0;
}
static void sl_add(slist *s, const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], name)) return; /* dedupe */
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 128;
        char **v = (char **)realloc(s->v, (size_t)nc * sizeof(char *));
        if (!v) return;
        s->v = v; s->cap = nc;
    }
    s->v[s->n++] = (char *)malloc(strlen(name) + 1);
    if (s->v[s->n - 1]) strcpy(s->v[s->n - 1], name);
    else s->n--;
}
static int cmpstr(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

#ifdef _WIN32
static int CALLBACK enum_font(const LOGFONTW *lf, const TEXTMETRICW *tm,
                              DWORD type, LPARAM lparam) {
    (void)tm; (void)type;
    slist *s = (slist *)lparam;
    if (lf->lfFaceName[0] && (lf->lfFaceName[0] != L'@')) {
        char name[256];
        int n = WideCharToMultiByte(CP_UTF8, 0, lf->lfFaceName, -1, name, sizeof name, NULL, NULL);
        if (n > 0) sl_add(s, name);
    }
    return 1; /* continue */
}
#endif

/* Parse a `fc-list : family` style line: families are comma-separated before
 * the first ':' (which introduces style/file). Handles `fc-list` output like
 *  "Noto Sans CJK SC,Noto Sans CJK:style=Regular" */
static void add_fc_line(slist *s, char *line) {
    char *colon = strchr(line, ':');
    if (colon) *colon = 0;
    char *p = line;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        if (*p) sl_add(s, p);
        if (!comma) break;
        p = comma + 1;
    }
}

int np_font_scan_families(char *out, size_t cap, size_t per_name) {
    if (!out || cap == 0 || per_name == 0) return -1;
    out[0] = 0;
    slist s; memset(&s, 0, sizeof s);

#ifdef _WIN32
    HDC dc = GetDC(NULL);
    if (dc) {
        LOGFONTW lf; memset(&lf, 0, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, enum_font, (LPARAM)&s, 0);
        ReleaseDC(NULL, dc);
    }
#elif !defined(__ANDROID__)
    /* Prefer fontconfig when available (real family names, incl. CJK).
     * Android's bionic libc has no popen(), so skip enumeration there. */
    bool got = false;
    FILE *f = popen("fc-list : family 2>/dev/null", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (n) add_fc_line(&s, line);
        }
        pclose(f);
        got = s.n > 0;
    }
    if (!got) {
        /* Fallback: scan common font directories for file names. */
        const char *dirs[] = {
            "/System/Library/Fonts", "/Library/Fonts",
            "/usr/share/fonts/truetype", "/usr/share/fonts/opentype",
            "/system/fonts", NULL
        };
        char cmd[512];
        for (int i = 0; dirs[i]; i++) {
            snprintf(cmd, sizeof cmd, "find '%s' -maxdepth 3 -type f \\( -iname '*.ttf' -o -iname '*.otf' -o -iname '*.ttc' \\) 2>/dev/null", dirs[i]);
            FILE *pf = popen(cmd, "r");
            if (!pf) continue;
            char line[1024];
            while (fgets(line, sizeof line, pf)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                const char *base = line;
                for (const char *q = line; *q; q++) if (*q == '/') base = q + 1;
                if (*base) sl_add(&s, base);
            }
            pclose(pf);
        }
    }
#endif

    if (s.n > 1) qsort(s.v, (size_t)s.n, sizeof(char *), cmpstr);
    size_t used = 0;
    int written = 0;
    for (int i = 0; i < s.n; i++) {
        size_t len = strlen(s.v[i]);
        if (len >= per_name) continue;
        if (used + len + 1 > cap) { written = -1; break; }
        memcpy(out + used, s.v[i], len + 1);
        used += len + 1;
        written++;
    }
    if (written >= 0) out[used] = 0; /* extra sentinel */
    sl_free(&s);
    return written;
}
