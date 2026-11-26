#ifndef NP_INDEX_H
#define NP_INDEX_H

/*
 * np_index.h - A4: local index + evaluation hooks.
 *
 * Local index avoids full workspace rescans on every semantic search:
 *   - text: file paths + symbol names (cheap in-memory token index)
 *   - binary: metadata (size, detected type, CRC32/SHA-256 prefix)
 * Evaluation hooks are local and DISABLEABLE: record accept/reject of AI
 * suggestions, integrity outcomes, task completion, etc. No telemetry is
 * ever uploaded; the app decides whether to collect at all.
 *
 * Also defines the golden-sample manifest structure used by R8/DC9/F9
 * regression gates (the samples themselves are test assets).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include "np_integrity.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------- local search index ---------------- */
typedef struct {
    char  path[260];
    char *symbols;       /* newline-separated, owned */
    size_t symbols_len;
    uint64_t mtime_hint;
} np_index_doc_t;

typedef struct np_local_index np_local_index_t;

np_local_index_t *np_index_create(void);
void np_index_destroy(np_local_index_t *idx);

/* Add/replace a document by path. Copies strings. */
bool np_index_upsert_text(np_local_index_t *idx, const char *path,
                          const char *symbols, uint64_t mtime_hint);
/* Add binary metadata. type is e.g. "ELF x86-64" / "SQLite3" / "OOXML". */
bool np_index_upsert_binary(np_local_index_t *idx, const char *path,
                            const void *head, size_t head_len,
                            uint64_t full_size, const char *type);

size_t np_index_count(const np_local_index_t *idx);
const np_index_doc_t *np_index_get(const np_local_index_t *idx, const char *path);

/* Search path + symbols for a query token (case-insensitive substring).
 * Returns number of matching paths written into out (each <= 260 bytes). */
size_t np_index_search(const np_local_index_t *idx, const char *query,
                       char out_paths[][260], size_t out_cap);

/* ---------------- evaluation hooks (local, disableable) ---------------- */
typedef enum {
    NP_METRIC_TEXT_SUGGEST_ACCEPT = 0,
    NP_METRIC_BYTE_SUGGEST_ACCEPT,
    NP_METRIC_TASK_ONESHOT,
    NP_METRIC_INTEGRITY_MISMATCH,   /* must stay 0 */
    NP_METRIC_BYTE_MISWRITE,        /* must stay 0 */
    NP_METRIC_LENGTH_RECOVER_FAIL,  /* must stay 0 */
    NP_METRIC_TOOL_BUDGET_OVERFLOW  /* must stay 0 */
} np_metric_t;

const char *np_metric_name(np_metric_t m);

typedef struct np_metrics np_metrics_t;
np_metrics_t *np_metrics_create(void);
void np_metrics_destroy(np_metrics_t *m);

/* Master switch: when disabled, record() is a no-op (local-only privacy). */
void np_metrics_set_enabled(np_metrics_t *m, bool enabled);
bool np_metrics_enabled(const np_metrics_t *m);

/* Record one event. total/accept semantics depend on metric:
 *   accept-type: total++ and accept if `ok`
 *   invariant-type (mismatch/miswrite/...): total++ and count++ if !ok */
void np_metrics_record(np_metrics_t *m, np_metric_t metric, bool ok);
uint64_t np_metrics_total(const np_metrics_t *m, np_metric_t metric);
uint64_t np_metrics_ok(const np_metrics_t *m, np_metric_t metric);
/* accept rate 0.0..1.0 for accept-type metrics; 0 if no samples. */
double np_metrics_rate(const np_metrics_t *m, np_metric_t metric);

/* ---------------- regression gate manifest (R8/DC9/F9) ---------------- */
typedef struct {
    char     id[80];          /* "r8-x64-push-rbp"                        */
    char     track_kind[16];  /* "disasm" / "decompile" / "format"        */
    char     arch[32];        /* "x86-64" / "arm64" / "loongarch64"       */
    char     sample_name[120];
    char     expected_hash[65];   /* SHA-256 of golden output              */
    char     expected_summary[200];
} np_golden_sample_t;

typedef struct np_gate np_gate_t;
np_gate_t *np_gate_create(void);
void np_gate_destroy(np_gate_t *g);
bool np_gate_add(np_gate_t *g, const np_golden_sample_t *s);
size_t np_gate_count(const np_gate_t *g);

/* Evaluate a run: for each sample present in `results`, compare its hash to
 * the golden hash. Returns pass rate; writes failing ids into out_fails. */
typedef struct { char id[80]; char hash[65]; } np_gate_result_t;
double np_gate_evaluate(const np_gate_t *g, const np_gate_result_t *results,
                        size_t nresults, char out_fails[][80], size_t out_cap,
                        size_t *out_nfails);

#ifdef __cplusplus
}
#endif
#endif /* NP_INDEX_H */
