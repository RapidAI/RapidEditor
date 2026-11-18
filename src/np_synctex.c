/* np_synctex.c - tiny SyncTeX reader (see np_synctex.h). */
#include "np_synctex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NP_STX_PATH_CAP 1024
#define NP_STX_MAX_FILES 128

typedef struct {
    int page, file, line, col;
    float x, y, w, h;
    char tag;                  /* h, v, g or k */
} np_stx_rec;

struct np_synctex {
    char *files[NP_STX_MAX_FILES];
    int nfiles;
    np_stx_rec *rec;
    int n, cap;
};

static char *stx_dupn(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static int stx_add_rec(np_synctex *stx, const np_stx_rec *r) {
    if (stx->n == stx->cap) {
        int cap = stx->cap ? stx->cap * 2 : 256;
        np_stx_rec *p = (np_stx_rec *)realloc(stx->rec, (size_t)cap * sizeof *p);
        if (!p) return 0;
        stx->rec = p;
        stx->cap = cap;
    }
    stx->rec[stx->n++] = *r;
    return 1;
}

/* Parse "tag<file>,<line>:<x>,<y>[:...]" after the tag character. */
static int stx_box(const char *p, int page, np_stx_rec *out) {
    char *comma = strchr(p, ',');
    char *colon = strchr(p, ':');
    if (!comma || !colon || comma > colon) return 0;
    out->file = (int)strtol(p, NULL, 10);
    out->line = (int)strtol(comma + 1, NULL, 10);
    if (out->file < 0 || out->line < 1) return 0;
    out->page = page;
    out->col = 1;
    out->x = strtof(colon + 1, NULL) / 65536.0f;
    const char *xy_comma = strchr(colon + 1, ',');
    if (!xy_comma) return 0;
    out->y = strtof(xy_comma + 1, NULL) / 65536.0f;
    const char *c2 = strchr(xy_comma + 1, ':');
    if (c2) {
        out->w = strtof(c2 + 1, NULL) / 65536.0f;
        const char *c3 = strchr(c2 + 1, ',');
        if (c3) out->h = strtof(c3 + 1, NULL) / 65536.0f;
    }
    if (!isfinite(out->x) || !isfinite(out->y)) return 0;
    return 1;
}

np_synctex *np_synctex_load(const char *path) {
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = 0;

    np_synctex *stx = (np_synctex *)calloc(1, sizeof *stx);
    if (!stx) { free(buf); return NULL; }

    int page_stack[64], npages = 0, page = 0;
    char *p = buf;
    while (*p) {
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        size_t len = (size_t)(e - p);
        if (len >= 2 && p[len - 1] == '\r') len--;
        char line[2048];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = *e ? e + 1 : e;

        if (!strncmp(line, "Input:", 6)) {
            const char *id = line + 6;
            const char *colon = strchr(id, ':');
            if (colon && colon > id && stx->nfiles < NP_STX_MAX_FILES) {
                int idx = (int)strtol(id, NULL, 10);
                const char *name = colon + 1;
                if (idx >= 0 && idx < NP_STX_MAX_FILES) {
                    free(stx->files[idx]);
                    stx->files[idx] = stx_dupn(name, strlen(name));
                    if (idx >= stx->nfiles) stx->nfiles = idx + 1;
                }
            }
            continue;
        }
        if (line[0] == '[') {
            if (npages < 64) page_stack[npages++] = page;
            page = (int)strtol(line + 1, NULL, 10);
            continue;
        }
        if (line[0] == ']' && npages > 0) {
            page = page_stack[--npages];
            continue;
        }
        char tag = line[0];
        if ((tag == 'h' || tag == 'g' || tag == 'k' || tag == 'v') &&
            line[1] >= '0' && line[1] <= '9') {
            np_stx_rec r;
            memset(&r, 0, sizeof r);
            r.tag = tag;
            if (stx_box(line + 1, page, &r)) stx_add_rec(stx, &r);
        }
    }
    free(buf);
    if (stx->n == 0) { np_synctex_free(stx); return NULL; }
    return stx;
}

void np_synctex_free(np_synctex *stx) {
    if (!stx) return;
    for (int i = 0; i < NP_STX_MAX_FILES; i++) free(stx->files[i]);
    free(stx->rec);
    free(stx);
}

int np_synctex_locate(np_synctex *stx, int page, float x, float y,
                      char *file, size_t file_cap, int *line, int *col) {
    if (!stx || !stx->rec || !file || file_cap == 0) return 0;
    const np_stx_rec *best = NULL;
    float best_d = 1e30f;
    for (int i = 0; i < stx->n; i++) {
        const np_stx_rec *r = &stx->rec[i];
        if (r->page != page) continue;
        float dx = 0, dy = 0;
        if ((r->tag == 'h' || r->tag == 'v') && r->w > 0 && r->h > 0) {
            if (x < r->x) dx = r->x - x;
            else if (x > r->x + r->w) dx = x - (r->x + r->w);
            if (y < r->y) dy = r->y - y;
            else if (y > r->y + r->h) dy = y - (r->y + r->h);
        } else {
            dx = fabsf(x - r->x);
            dy = fabsf(y - r->y);
        }
        float d = dx + dy * 0.9f;
        if ((r->tag == 'h' || r->tag == 'v') && d == 0) d = -1.0f;  /* containing box wins */
        if (d < best_d) { best_d = d; best = r; }
    }
    if (!best) return 0;
    file[0] = 0;
    if (best->file >= 0 && best->file < NP_STX_MAX_FILES && stx->files[best->file])
        snprintf(file, file_cap, "%s", stx->files[best->file]);
    if (line) *line = best->line;
    if (col) *col = best->col;
    return file[0] ? 1 : 0;
}
