#ifndef NP_BYTE_DIFF_H
#define NP_BYTE_DIFF_H

/*
 * np_bytediff.h - H7: byte-level diff between two binary versions.
 *
 * Produces offset-aligned byte hunks (REPLACE / INSERT / DELETE) suitable for
 * feeding the shared B3 patch-review panel. Read-only with respect to both
 * inputs; the caller decides whether to commit accepted hunks through
 * np_bytepatch (one undo group => one Ctrl+Z rollback).
 *
 * Correctness gate: np_bytediff_verify() replays every hunk onto `before` and
 * checks byte-for-byte equality with `after`. compute() self-verifies and
 * degrades to a single coarse REPLACE hunk (fuzzy=true) if the fine diff ever
 * fails to replay -- so a returned result is always replayable.
 *
 * UI-free C99 module, unit-testable without CUI.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_integrity.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NP_BD_REPLACE = 0,   /* [before_off,before_off+before_len) <- after bytes */
    NP_BD_INSERT,        /* at before_off, insert after bytes */
    NP_BD_DELETE         /* remove [before_off,before_off+before_len) */
} np_bd_kind_t;

const char *np_bd_kind_name(np_bd_kind_t k);

/* Per-hunk bounded preview bytes for the review panel / hex jump. */
#define NP_BD_PREVIEW 16

typedef struct {
    np_bd_kind_t kind;
    size_t       before_off;   /* offset in the before buffer */
    size_t       before_len;   /* bytes consumed from before (INSERT: 0) */
    size_t       after_off;    /* offset in the after buffer */
    size_t       after_len;    /* bytes emitted into after (DELETE: 0) */
    unsigned char before_bytes[NP_BD_PREVIEW];
    unsigned char after_bytes[NP_BD_PREVIEW];
    size_t       before_prev_n;
    size_t       after_prev_n;
} np_bd_hunk_t;

typedef struct {
    np_bd_hunk_t *hunks;       /* owned; release with np_bytediff_free() */
    size_t        nhunks;
    size_t        before_len, after_len;
    uint32_t      crc_before, crc_after;
    bool          fuzzy;       /* true => coarse single-hunk degradation */
} np_bd_result_t;

/* Compute diff. Always fills out (zeroed on entry); returns false only on
 * invalid arguments / allocation failure. */
bool np_bytediff_compute(const void *before, size_t blen,
                         const void *after, size_t alen,
                         np_bd_result_t *out);

void np_bytediff_free(np_bd_result_t *out);

/* Replay result onto `before`, using `after` for the replacement bytes.
 * Writes at most out_cap bytes; out_len receives the produced length. */
bool np_bytediff_apply(const void *before, size_t blen,
                       const void *after, const np_bd_result_t *res,
                       unsigned char *out, size_t out_cap, size_t *out_len);

/* Correctness gate: replay must equal `after` byte-for-byte. */
bool np_bytediff_verify(const void *before, size_t blen,
                        const np_bd_result_t *res,
                        const void *after, size_t alen);

#ifdef __cplusplus
}
#endif
#endif /* NP_BYTE_DIFF_H */
