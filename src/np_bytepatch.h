#ifndef NP_BYTEPATCH_H
#define NP_BYTEPATCH_H

/*
 * np_bytepatch.h - H1: byte-level patch + undo foundation for the binary
 * track. The vendored cui_hex_view only does in-place equal-length edits
 * and has no undo stack. This module supplies the missing substrate:
 *
 *   - copy-on-write snapshot of the byte buffer before a patch
 *   - variable-length byte ops (insert/delete change file size)
 *   - a bounded undo/redo stack; one AI Job = one undo group
 *   - length restoration on revert (the document's "length recovery 100%")
 *
 * It is independent of CUI so it can be unit-tested, then wrapped by the
 * binary pane. It composes with np_patch (A3) and np_integrity (H2).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NP_BP_OK = 0,
    NP_BP_ERR_ARG,
    NP_BP_ERR_RANGE,
    NP_BP_ERR_ALLOC,
    NP_BP_ERR_NO_UNDO,
    NP_BP_ERR_NO_REDO
} np_bp_result_t;

const char *np_bp_result_name(np_bp_result_t r);

typedef struct np_bytepatch np_bytepatch_t;

/* Create a byte document from an initial buffer. Copies the bytes. */
np_bytepatch_t *np_bytepatch_create(const void *data, size_t len);
void np_bytepatch_destroy(np_bytepatch_t *bp);

/* Read current content (pointer valid until the next mutating call). */
const unsigned char *np_bytepatch_data(const np_bytepatch_t *bp, size_t *out_len);

/* Start a new undo group (one AI Job). Snapshots current state. */
bool np_bytepatch_begin_group(np_bytepatch_t *bp);

/* Apply a byte op inside the current group:
 *   REPLACE: [offset, offset+len) <- bytes[0..bytes_len)  (len may differ,
 *            so this can grow/shrink the document)
 *   INSERT : at offset, insert bytes (len ignored, pass 0)
 *   DELETE : remove [offset, offset+len) (bytes ignored)
 * Returns NP_BP_OK on success. */
np_bp_result_t np_bytepatch_replace(np_bytepatch_t *bp, size_t offset, size_t len,
                                    const unsigned char *bytes, size_t bytes_len);
np_bp_result_t np_bytepatch_insert(np_bytepatch_t *bp, size_t offset,
                                   const unsigned char *bytes, size_t bytes_len);
np_bp_result_t np_bytepatch_delete(np_bytepatch_t *bp, size_t offset, size_t len);

/* End the current group; returns false if nothing changed. */
bool np_bytepatch_end_group(np_bytepatch_t *bp);

/* Undo/redo the whole last group. Restores exact bytes AND length. */
bool np_bytepatch_undo(np_bytepatch_t *bp);
bool np_bytepatch_redo(np_bytepatch_t *bp);
bool np_bytepatch_can_undo(const np_bytepatch_t *bp);
bool np_bytepatch_can_redo(const np_bytepatch_t *bp);

/* Discard redo history when a new edit is made after an undo. */
void np_bytepatch_clear_redo(np_bytepatch_t *bp);

#ifdef __cplusplus
}
#endif
#endif /* NP_BYTEPATCH_H */
