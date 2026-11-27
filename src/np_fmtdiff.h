#ifndef NP_FMTDIFF_H
#define NP_FMTDIFF_H

/*
 * np_fmtdiff.h - F7: structured format / schema diff with per-block review.
 *
 * Two versions of a document or database are reduced to a list of typed
 * elements keyed by a structural path (F2 node path / `sheet!cell` /
 * `table.col` / `table@index`). The diff engine aligns by path, classifies
 * ADD / DEL / MOD, and feeds a review list where every change can be
 * individually accepted or rejected (B3 semantics, kept UI-free here).
 *
 * The engine does not parse OOXML/SQLite itself: callers extract elements
 * (F2 structure tree / F3 schema) and pass them in. Zero hard dependencies.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

#define NP_FD_PATH_CAP 256
#define NP_FD_TEXT_CAP 512
#define NP_FD_LABEL_CAP 160
#define NP_FD_MAX_ELEMS 4096
#define NP_FD_MAX_CHANGES 4096

typedef enum {
    NP_FD_ADD = 0,
    NP_FD_DEL,
    NP_FD_MOD
} np_fd_op_t;

const char *np_fd_op_name(np_fd_op_t op);

typedef enum {
    NP_FD_PENDING = 0,
    NP_FD_ACCEPTED,
    NP_FD_REJECTED
} np_fd_state_t;

const char *np_fd_state_name(np_fd_state_t s);

/* One structural element. `path` is the stable key; `text` is the compared
 * payload; `label` is a human-readable description for the review panel. */
typedef struct {
    char path[NP_FD_PATH_CAP];
    char text[NP_FD_TEXT_CAP];
    char label[NP_FD_LABEL_CAP];
} np_fd_elem_t;

typedef struct {
    np_fd_elem_t *items;
    size_t count, cap;
} np_fd_set_t;

void np_fd_set_init(np_fd_set_t *s);
void np_fd_set_free(np_fd_set_t *s);
/* Append a copy; false on cap/bounds failure. */
bool np_fd_set_add(np_fd_set_t *s, const char *path, const char *text, const char *label);
const np_fd_elem_t *np_fd_set_find(const np_fd_set_t *s, const char *path);

/* One diff change. `before`/`after` carry the compared text and label; the
 * `path` doubles as the format-track citation key (file#struct-path). */
typedef struct {
    np_fd_op_t    op;
    np_fd_state_t state;
    char          path[NP_FD_PATH_CAP];
    char          before_text[NP_FD_TEXT_CAP];
    char          after_text[NP_FD_TEXT_CAP];
    char          before_label[NP_FD_LABEL_CAP];
    char          after_label[NP_FD_LABEL_CAP];
} np_fd_change_t;

typedef struct {
    np_fd_change_t *items;
    size_t count, cap;
    char            err[160];
} np_fd_result_t;

void np_fd_result_init(np_fd_result_t *r);
void np_fd_result_free(np_fd_result_t *r);

/* Compute an ordered diff (sorted by path). Same path + different payload =
 * MOD; path only in A = DEL; only in B = ADD. Duplicate paths in one set are
 * rejected (fail-closed) rather than silently dropped. */
bool np_fd_diff(const np_fd_set_t *a, const np_fd_set_t *b, np_fd_result_t *out);

/* Per-block review state machine. */
bool np_fd_accept(np_fd_result_t *r, size_t idx);
bool np_fd_reject(np_fd_result_t *r, size_t idx);
size_t np_fd_count_by_state(const np_fd_result_t *r, np_fd_state_t s);

/* Render the accepted changes as a canonical "patch lines" buffer, one change
 * per line: "OP<TAB>path<TAB>after_text". Used to feed B3 review / audit. */
size_t np_fd_render_accepted(const np_fd_result_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_FMTDIFF_H */
