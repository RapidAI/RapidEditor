/* np_context.c - A2 three-track context engine (see header). */
#include "np_context.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

np_ctx_budget_t np_ctx_default_budget(void) {
    np_ctx_budget_t b;
    b.max_chars = 24000;   /* ~6k tokens */
    b.max_bytes = 4096;    /* binary byte window */
    b.max_paths = 24;
    b.align_bytes = 16;
    return b;
}

/* ---------------- .aiignore ---------------- */
struct np_ignore {
    char **patterns;
    size_t count;
};

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Simple glob: supports '*' wildcard, case-insensitive, matched against the
 * whole path or its basename. Enough for .aiignore semantics in S0. */
static bool glob_match(const char *pat, const char *path) {
    const char *name = strrchr(path, '/');
    if (!name) name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    const char *targets[2] = { path, name };
    for (int t = 0; t < 2; ++t) {
        const char *s = targets[t];
        const char *p = pat;
        const char *star_pat = NULL;   /* pattern position right after last '*' */
        const char *star_str = NULL;   /* string position that '*' started at  */
        while (*s) {
            if (*p == '*') {
                star_pat = ++p;        /* everything after the '*'            */
                star_str = s;
                continue;
            }
            if (*p && lower(*p) == lower(*s)) { p++; s++; continue; }
            if (star_pat) {            /* backtrack: consume one more char by '*' */
                star_str++;
                s = star_str;
                p = star_pat;
                continue;
            }
            break;
        }
        while (*p == '*') p++;
        if (*p == '\0' && *s == '\0') return true;
        /* allow trailing '*' to absorb the rest */
        if (star_pat && *p == '\0') return true;
    }
    return false;
}

np_ignore_t *np_ignore_create(const char *patterns_text) {
    np_ignore_t *ig = (np_ignore_t *)calloc(1, sizeof(*ig));
    if (!ig) return NULL;
    if (!patterns_text) return ig;
    char *copy = (char *)malloc(strlen(patterns_text) + 1);
    if (!copy) { free(ig); return NULL; }
    strcpy(copy, patterns_text);
    /* hand-rolled line split (strtok_r/strtok_s differ across MSVC/POSIX) */
    char *cur = copy;
    while (cur && *cur) {
        char *line = cur;
        char *eol = strpbrk(cur, "\r\n");
        if (eol) { *eol = '\0'; cur = eol + 1; while (*cur == '\r' || *cur == '\n') cur++; }
        else cur = NULL;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        char **p = (char **)realloc(ig->patterns, (ig->count + 1) * sizeof(*p));
        if (!p) break;
        ig->patterns = p;
        ig->patterns[ig->count] = (char *)malloc(strlen(line) + 1);
        if (ig->patterns[ig->count]) {
            strcpy(ig->patterns[ig->count], line);
            ig->count++;
        }
    }
    free(copy);
    return ig;
}

void np_ignore_destroy(np_ignore_t *ig) {
    if (!ig) return;
    for (size_t i = 0; i < ig->count; ++i) free(ig->patterns[i]);
    free(ig->patterns);
    free(ig);
}

bool np_ignore_matches(const np_ignore_t *ig, const char *path) {
    if (!ig || !path) return false;
    for (size_t i = 0; i < ig->count; ++i)
        if (ig->patterns[i] && glob_match(ig->patterns[i], path)) return true;
    return false;
}

/* ---------------- track-specific limits ---------------- */
size_t np_ctx_text_limit(size_t len, size_t max_chars) {
    /* Hard char/token-proxy cap. The UI shows the exact cut position in the
     * audit summary; callers that need newline alignment can pre-segment. */
    return len < max_chars ? len : max_chars;
}

size_t np_ctx_binary_limit(size_t len, size_t max_bytes, size_t align) {
    if (align == 0) align = 16;
    if (len <= max_bytes) return len;
    size_t cut = max_bytes - (max_bytes % align);
    return cut;
}

/* ---------------- context build ---------------- */
/* Append a line to the bounded audit buffer without strncat (avoids MSVC
 * C4996 and is correct on all C99 toolchains). */
static void audit_append(char *dst, size_t cap, const char *line) {
    size_t used = strlen(dst);
    if (used + 1 >= cap) return;
    size_t room = cap - used - 1;
    size_t n = strlen(line);
    if (n > room) n = room;
    memcpy(dst + used, line, n);
    dst[used + n] = '\0';
}

bool np_ctx_build(const np_ctx_fragment_t *frags, size_t nfrags,
                  const np_ctx_budget_t *budget_in, const np_ignore_t *ig,
                  char **out_text, size_t *out_len, np_ctx_audit_t *audit) {
    if (!out_text || !out_len) return false;
    *out_text = NULL; *out_len = 0;
    np_ctx_budget_t b = budget_in ? *budget_in : np_ctx_default_budget();

    if (audit) memset(audit, 0, sizeof(*audit));
    if (audit) snprintf(audit->audit, sizeof(audit->audit), "Context pack:\n");

    /* combined buffer with a modest starting capacity */
    size_t cap = 4096, used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return false;

    size_t total_budget = b.max_chars;  /* overall combined cap (token proxy) */
    for (size_t i = 0; i < nfrags; ++i) {
        const np_ctx_fragment_t *f = &frags[i];
        if (!f || !f->text) continue;
        if (np_ignore_matches(ig, f->source)) {
            if (audit) {
                audit->fragments_ignored++;
                char line[512];
                snprintf(line, sizeof(line), "  [ignored] %s (.aiignore)\n", f->source);
                audit_append(audit->audit, sizeof(audit->audit), line);
            }
            continue;
        }
        size_t keep = f->len;
        if (f->track == NP_TRACK_TEXT) {
            keep = np_ctx_text_limit(f->len, b.max_chars);
        } else if (f->track == NP_TRACK_BINARY) {
            keep = np_ctx_binary_limit(f->len, b.max_bytes, b.align_bytes);
        } else if (f->track == NP_TRACK_FORMAT) {
            /* format fragments are already path-scoped; cap chars as a proxy */
            keep = np_ctx_text_limit(f->len, b.max_chars);
        }
        /* combined overall budget */
        if (used + keep > total_budget) keep = used < total_budget ? total_budget - used : 0;
        bool truncated = keep < f->len;
        if (truncated && audit) {
            audit->fragments_truncated++;
            audit->bytes_dropped += f->len - keep;
        }
        if (audit) {
            audit->fragments_included++;
            char line[512];
            snprintf(line, sizeof(line), "  [%s] %s : %zu bytes%s\n",
                     np_track_name(f->track), f->source, keep,
                     truncated ? " (TRUNCATED)" : "");
            audit_append(audit->audit, sizeof(audit->audit), line);
        }
        if (keep) {
            if (used + keep + 2 > cap) {
                size_t nc = used + keep + 2;
                char *p = (char *)realloc(buf, nc);
                if (!p) { free(buf); return false; }
                buf = p; cap = nc;
            }
            memcpy(buf + used, f->text, keep);
            used += keep;
            buf[used++] = '\n';
            if (audit) audit->total_chars += keep;
        }
    }
    if (used < cap) buf[used] = '\0';
    *out_text = buf;
    *out_len = used;
    return true;
}

void np_ctx_free_text(char *text) { free(text); }
