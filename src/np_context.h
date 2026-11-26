#ifndef NP_CONTEXT_H
#define NP_CONTEXT_H

/*
 * np_context.h - A2: three-track context engine.
 *
 * Context is an auditable asset. Before anything is sent to the model:
 *   - each track has its OWN budget: text by tokens/chars at semantic
 *     boundaries, binary by byte window at 16-byte line boundaries,
 *     format by structure-path fragments + masked samples.
 *   - .aiignore patterns filter files out entirely.
 *   - an audit summary records what is included, how big, and where it was
 *     truncated, so the UI can show the user before sending.
 *   - only selections are sent by default; full-file needs confirmation.
 *
 * Pure C99, string-based, unit-testable without any LLM.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    size_t max_chars;        /* text track char budget (token proxy)       */
    size_t max_bytes;        /* binary track byte-window budget           */
    size_t max_paths;        /* format track number of structure paths    */
    size_t align_bytes;      /* binary truncation alignment (default 16)  */
} np_ctx_budget_t;

np_ctx_budget_t np_ctx_default_budget(void);

/* Parsed .aiignore-style glob patterns (line-separated). */
typedef struct np_ignore np_ignore_t;
np_ignore_t *np_ignore_create(const char *patterns_text);
void np_ignore_destroy(np_ignore_t *ig);
/* Returns true if `path` matches any ignore pattern. */
bool np_ignore_matches(const np_ignore_t *ig, const char *path);

/* A context fragment contributed by one source. */
typedef struct {
    np_track_t track;
    char       source[260];  /* file path / citation                      */
    char      *text;         /* owned UTF-8 content (binary as hex text)  */
    size_t     len;
} np_ctx_fragment_t;

/* Build a context pack from fragments. For each fragment:
 *   - skip if source matches .aiignore
 *   - truncate to the track's budget at a semantic boundary
 *   - record truncation in the audit summary
 * Produces a single combined UTF-8 buffer in `out` (owned, caller frees). */
typedef struct {
    size_t     total_chars;
    size_t     fragments_included;
    size_t     fragments_ignored;
    size_t     fragments_truncated;
    size_t     bytes_dropped;
    char       audit[2048];  /* human-readable "what was included/cut"    */
} np_ctx_audit_t;

bool np_ctx_build(const np_ctx_fragment_t *frags, size_t nfrags,
                  const np_ctx_budget_t *budget, const np_ignore_t *ig,
                  char **out_text, size_t *out_len, np_ctx_audit_t *audit);
void np_ctx_free_text(char *text);

/* Track-specific truncation lengths (exported for direct testing).
 * Returns the number of chars/words to keep from a fragment of `len` bytes. */
size_t np_ctx_text_limit(size_t len, size_t max_chars);
size_t np_ctx_binary_limit(size_t len, size_t max_bytes, size_t align);

#ifdef __cplusplus
}
#endif
#endif /* NP_CONTEXT_H */
