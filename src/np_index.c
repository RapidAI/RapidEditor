/* np_index.c - A4 local index + evaluation hooks (see header). */
#include "np_index.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NP_IDX_MAX_DOCS 2048
#define NP_GATE_MAX 512

/* ---------------- local index ---------------- */
struct np_local_index {
    np_index_doc_t docs[NP_IDX_MAX_DOCS];
    size_t count;
};

np_local_index_t *np_index_create(void) {
    return (np_local_index_t *)calloc(1, sizeof(np_local_index_t));
}
void np_index_destroy(np_local_index_t *idx) {
    if (!idx) return;
    for (size_t i = 0; i < idx->count; ++i) free(idx->docs[i].symbols);
    free(idx);
}

static long idx_find(const np_local_index_t *idx, const char *path) {
    if (!idx || !path) return -1;
    for (size_t i = 0; i < idx->count; ++i)
        if (strcmp(idx->docs[i].path, path) == 0) return (long)i;
    return -1;
}

static bool set_symbols(np_index_doc_t *d, const char *symbols) {
    free(d->symbols);
    d->symbols = NULL; d->symbols_len = 0;
    if (symbols) {
        d->symbols_len = strlen(symbols);
        d->symbols = (char *)malloc(d->symbols_len + 1);
        if (!d->symbols) { d->symbols_len = 0; return false; }
        memcpy(d->symbols, symbols, d->symbols_len + 1);
    }
    return true;
}

bool np_index_upsert_text(np_local_index_t *idx, const char *path,
                          const char *symbols, uint64_t mtime_hint) {
    if (!idx || !path) return false;
    long at = idx_find(idx, path);
    if (at < 0) {
        if (idx->count >= NP_IDX_MAX_DOCS) return false;
        at = (long)idx->count++;
        memset(&idx->docs[at], 0, sizeof(idx->docs[at]));
        snprintf(idx->docs[at].path, sizeof(idx->docs[at].path), "%s", path);
    }
    idx->docs[at].mtime_hint = mtime_hint;
    return set_symbols(&idx->docs[at], symbols);
}

bool np_index_upsert_binary(np_local_index_t *idx, const char *path,
                            const void *head, size_t head_len,
                            uint64_t full_size, const char *type) {
    if (!idx || !path) return false;
    long at = idx_find(idx, path);
    if (at < 0) {
        if (idx->count >= NP_IDX_MAX_DOCS) return false;
        at = (long)idx->count++;
        memset(&idx->docs[at], 0, sizeof(idx->docs[at]));
        snprintf(idx->docs[at].path, sizeof(idx->docs[at].path), "%s", path);
    }
    /* metadata encoded into the symbols field: "size=<n> sha=<crc> type=<t>" */
    char meta[512];
    snprintf(meta, sizeof(meta), "size=%llu crc=%08X type=%s",
             (unsigned long long)full_size, np_crc32(head, head_len),
             type ? type : "unknown");
    idx->docs[at].mtime_hint = full_size;
    return set_symbols(&idx->docs[at], meta);
}

size_t np_index_count(const np_local_index_t *idx) { return idx ? idx->count : 0; }
const np_index_doc_t *np_index_get(const np_local_index_t *idx, const char *path) {
    long at = idx_find(idx, path);
    return at >= 0 ? &idx->docs[at] : NULL;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool contains_nocase(const char *hay, const char *needle) {
    if (!needle || !*needle) return false;
    size_t nl = strlen(needle), hl = strlen(hay);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; ++i) {
        bool ok = true;
        for (size_t j = 0; j < nl; ++j)
            if (lower(hay[i+j]) != lower(needle[j])) { ok = false; break; }
        if (ok) return true;
    }
    return false;
}

size_t np_index_search(const np_local_index_t *idx, const char *query,
                       char out_paths[][260], size_t out_cap) {
    if (!idx || !query || !out_paths || out_cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < idx->count && n < out_cap; ++i) {
        const np_index_doc_t *d = &idx->docs[i];
        if (contains_nocase(d->path, query) ||
            (d->symbols && contains_nocase(d->symbols, query)))
            snprintf(out_paths[n++], 260, "%s", d->path);
    }
    return n;
}

/* ---------------- metrics ---------------- */
const char *np_metric_name(np_metric_t m) {
    switch (m) {
        case NP_METRIC_TEXT_SUGGEST_ACCEPT:   return "text_accept";
        case NP_METRIC_BYTE_SUGGEST_ACCEPT:   return "byte_accept";
        case NP_METRIC_TASK_ONESHOT:          return "task_oneshot";
        case NP_METRIC_INTEGRITY_MISMATCH:    return "integrity_mismatch";
        case NP_METRIC_BYTE_MISWRITE:         return "byte_miswrite";
        case NP_METRIC_LENGTH_RECOVER_FAIL:   return "length_recover_fail";
        case NP_METRIC_TOOL_BUDGET_OVERFLOW:  return "tool_budget_overflow";
        default:                              return "unknown";
    }
}

typedef struct { uint64_t total, ok; } metric_counter_t;

struct np_metrics {
    bool enabled;
    metric_counter_t c[16];
};

np_metrics_t *np_metrics_create(void) {
    np_metrics_t *m = (np_metrics_t *)calloc(1, sizeof(*m));
    if (m) m->enabled = true;
    return m;
}
void np_metrics_destroy(np_metrics_t *m) { free(m); }
void np_metrics_set_enabled(np_metrics_t *m, bool enabled) { if (m) m->enabled = enabled; }
bool np_metrics_enabled(const np_metrics_t *m) { return m && m->enabled; }

void np_metrics_record(np_metrics_t *m, np_metric_t metric, bool ok) {
    if (!m || !m->enabled) return;
    if ((int)metric < 0 || (int)metric >= 16) return;
    metric_counter_t *c = &m->c[metric];
    c->total++;
    /* For invariant metrics, `ok` means the invariant held; count failures */
    if (metric >= NP_METRIC_INTEGRITY_MISMATCH) {
        if (!ok) c->ok++;   /* ok counts violations here */
    } else {
        if (ok) c->ok++;
    }
}
uint64_t np_metrics_total(const np_metrics_t *m, np_metric_t metric) {
    return (m && (int)metric>=0 && (int)metric<16) ? m->c[metric].total : 0;
}
uint64_t np_metrics_ok(const np_metrics_t *m, np_metric_t metric) {
    return (m && (int)metric>=0 && (int)metric<16) ? m->c[metric].ok : 0;
}
double np_metrics_rate(const np_metrics_t *m, np_metric_t metric) {
    uint64_t t = np_metrics_total(m, metric);
    if (!t) return 0.0;
    return (double)np_metrics_ok(m, metric) / (double)t;
}

/* ---------------- regression gate ---------------- */
struct np_gate {
    np_golden_sample_t samples[NP_GATE_MAX];
    size_t count;
};

np_gate_t *np_gate_create(void) { return (np_gate_t *)calloc(1, sizeof(np_gate_t)); }
void np_gate_destroy(np_gate_t *g) { free(g); }

bool np_gate_add(np_gate_t *g, const np_golden_sample_t *s) {
    if (!g || !s || g->count >= NP_GATE_MAX) return false;
    g->samples[g->count++] = *s;
    return true;
}
size_t np_gate_count(const np_gate_t *g) { return g ? g->count : 0; }

double np_gate_evaluate(const np_gate_t *g, const np_gate_result_t *results,
                        size_t nresults, char out_fails[][80], size_t out_cap,
                        size_t *out_nfails) {
    if (!g || g->count == 0) return 1.0;
    size_t pass = 0, fails = 0;
    for (size_t i = 0; i < g->count; ++i) {
        const np_golden_sample_t *s = &g->samples[i];
        const np_gate_result_t *r = NULL;
        for (size_t k = 0; k < nresults; ++k)
            if (strcmp(results[k].id, s->id) == 0) { r = &results[k]; break; }
        bool ok = r && strcmp(r->hash, s->expected_hash) == 0;
        if (ok) pass++;
        else if (out_fails && fails < out_cap) {
            snprintf(out_fails[fails++], 80, "%s", s->id);
        }
    }
    if (out_nfails) {
        if (!out_fails) { /* count all failures even without output buffer */
            size_t nf = 0;
            for (size_t i = 0; i < g->count; ++i) {
                bool found = false;
                for (size_t k = 0; k < nresults; ++k)
                    if (strcmp(results[k].id, g->samples[i].id) == 0 &&
                        strcmp(results[k].hash, g->samples[i].expected_hash) == 0)
                        { found = true; break; }
                if (!found) nf++;
            }
            *out_nfails = nf;
        } else *out_nfails = fails;
    }
    return (double)pass / (double)g->count;
}
