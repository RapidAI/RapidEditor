#ifndef NP_PATCH_H
#define NP_PATCH_H

/*
 * np_patch.h - A3: three-track patch protocol.
 *
 * One PatchSet is one Review unit and one undo group. It holds an ordered
 * list of hunks. Each hunk belongs to exactly one track and uses that
 * track's PRIMITIVE - the primitives are never mixed:
 *
 *   text   : {line range, text}          - line replace / insert / delete
 *   binary : {offset, len, bytes, op}    - byte replace / insert / delete
 *              (insert/delete are variable-length => file size may change)
 *   format : {part, path, op, payload}   - XML part edit or SQL patch;
 *              the actual structured writer is F8 (S2). S0 defines the
 *              protocol and a validation gate only.
 *
 * Every hunk binds a base_revision; applying to a stale buffer is rejected.
 * Apply builds a copy-on-write snapshot so a whole PatchSet reverts with
 * one undo (H1 for binary, text revision for text).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NP_PATCH_OK = 0,
    NP_PATCH_ERR_ARG,
    NP_PATCH_ERR_REVISION,   /* base_revision mismatch (stale patch)     */
    NP_PATCH_ERR_RANGE,      /* offset/line outside buffer               */
    NP_PATCH_ERR_TRACK,      /* hunk applied to wrong track buffer       */
    NP_PATCH_ERR_NEEDS_F8,   /* format write needs structured writer      */
    NP_PATCH_ERR_ALLOC
} np_patch_result_t;

const char *np_patch_result_name(np_patch_result_t r);

/* A text hunk addresses a half-open line range [line_first, line_last).
 * For INSERT, line_first == line_last and lines are inserted before it. */
typedef struct {
    size_t      line_first;
    size_t      line_last;
    const char *text;       /* UTF-8, may contain '\n'; owned by caller   */
    size_t      text_len;
} np_text_hunk_t;

/* A byte hunk addresses a half-open byte range [offset, offset+len).
 * REPLACE keeps len, INSERT uses len==0, DELETE uses bytes==NULL. */
typedef struct {
    size_t           offset;
    size_t           len;
    const unsigned char *bytes;
    size_t           bytes_len;
} np_byte_hunk_t;

/* A format hunk is a structured edit. S0 validates + records it; the
 * deterministic writer (ZIP repack / SQL transaction) lands in F8. */
typedef struct {
    char        part[200];   /* e.g. "word/document.xml" or "app.db"     */
    char        path[256];   /* e.g. "/w:p[42]" or "users.email"         */
    const char *payload;     /* XML fragment or SQL patch                */
    size_t      payload_len;
} np_format_hunk_t;

typedef struct {
    np_track_t     track;
    np_patch_op_t  op;
    uint64_t       base_revision;
    union {
        np_text_hunk_t   text;
        np_byte_hunk_t   byte;
        np_format_hunk_t format;
    } u;
} np_patch_hunk_t;

/* --- mutable buffer -------------------------------------------------- */
/* Owns a byte buffer + a revision counter. Text tracks store UTF-8 bytes;
 * line hunks are resolved to byte ranges at apply time. */
typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
    uint64_t       revision;
} np_patch_buffer_t;

void  np_patch_buffer_init(np_patch_buffer_t *b);
void  np_patch_buffer_free(np_patch_buffer_t *b);
bool  np_patch_buffer_set(np_patch_buffer_t *b, const void *data, size_t len);
/* Line helpers for the text track. */
size_t np_patch_line_count(const np_patch_buffer_t *b);
size_t np_patch_line_byte_start(const np_patch_buffer_t *b, size_t line);
size_t np_patch_line_byte_end(const np_patch_buffer_t *b, size_t line);

/* --- patch set ------------------------------------------------------- */
typedef struct np_patch_set np_patch_set_t;

np_patch_set_t *np_patch_set_create(uint64_t base_revision);
void np_patch_set_destroy(np_patch_set_t *ps);
uint64_t np_patch_set_revision(const np_patch_set_t *ps);
size_t np_patch_set_count(const np_patch_set_t *ps);
const np_patch_hunk_t *np_patch_set_hunk(const np_patch_set_t *ps, size_t i);

/* Append hunks. The set stores a deep copy, so caller buffers may go away. */
np_patch_result_t np_patch_add_text(np_patch_set_t *ps, np_patch_op_t op,
                                    size_t line_first, size_t line_last,
                                    const char *text, size_t text_len);
np_patch_result_t np_patch_add_byte(np_patch_set_t *ps, np_patch_op_t op,
                                    size_t offset, size_t len,
                                    const unsigned char *bytes, size_t bytes_len);
np_patch_result_t np_patch_add_format(np_patch_set_t *ps, np_patch_op_t op,
                                      const char *part, const char *path,
                                      const char *payload, size_t payload_len);

/* Validate every hunk against `b` WITHOUT modifying it. For format hunks,
 * validation checks protocol fields and returns NP_PATCH_ERR_NEEDS_F8 to
 * signal that a deterministic writer is required before writing. */
np_patch_result_t np_patch_validate(const np_patch_set_t *ps, np_track_t track,
                                    const np_patch_buffer_t *b);

/* Apply all hunks atomically. On success the whole set is applied and the
 * buffer revision advances by 1. On any error the buffer is left unchanged
 * and the buffer is NOT modified. A private undo snapshot is saved inside
 * the set so a single np_patch_revert undoes the entire set. */
np_patch_result_t np_patch_apply(np_patch_set_t *ps, np_track_t track,
                                 np_patch_buffer_t *b);

/* Restore the buffer to the state before the last successful apply.
 * Returns false if there is nothing to revert. */
bool np_patch_revert(np_patch_set_t *ps, np_patch_buffer_t *b);

#ifdef __cplusplus
}
#endif
#endif /* NP_PATCH_H */
