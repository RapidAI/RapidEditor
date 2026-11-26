/* np_bytepatch.c - H1 byte patch + undo foundation (see np_bytepatch.h). */
#include "np_bytepatch.h"

#include <stdlib.h>
#include <string.h>

#define NP_BP_MAX_HISTORY 64

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} bp_snapshot_t;

struct np_bytepatch {
    unsigned char *data;
    size_t         len;
    size_t         cap;

    bool           in_group;
    bp_snapshot_t  group_before;   /* state at begin_group()            */
    bool           group_changed;

    bp_snapshot_t *undo_stack;
    int            undo_n, undo_cap;
    bp_snapshot_t *redo_stack;
    int            redo_n, redo_cap;
};

const char *np_bp_result_name(np_bp_result_t r) {
    switch (r) {
        case NP_BP_OK:         return "ok";
        case NP_BP_ERR_ARG:    return "arg";
        case NP_BP_ERR_RANGE:  return "range";
        case NP_BP_ERR_ALLOC:  return "alloc";
        case NP_BP_ERR_NO_UNDO:return "no-undo";
        case NP_BP_ERR_NO_REDO:return "no-redo";
        default:               return "unknown";
    }
}

static bool snap_copy(bp_snapshot_t *s, const unsigned char *data, size_t len) {
    s->data = NULL; s->len = len; s->cap = 0;
    if (len) {
        s->data = (unsigned char *)malloc(len);
        if (!s->data) { s->len = 0; return false; }
        memcpy(s->data, data, len);
        s->cap = len;
    }
    return true;
}
static void snap_free(bp_snapshot_t *s) {
    free(s->data); s->data = NULL; s->len = s->cap = 0;
}
static bool snap_restore(unsigned char **data, size_t *len, size_t *cap,
                         const bp_snapshot_t *s) {
    if (s->len > *cap) {
        unsigned char *p = (unsigned char *)realloc(*data, s->len ? s->len : 1);
        if (!p) return false;
        *data = p; *cap = s->len;
    }
    if (s->len) memcpy(*data, s->data, s->len);
    *len = s->len;
    return true;
}

np_bytepatch_t *np_bytepatch_create(const void *data, size_t len) {
    np_bytepatch_t *bp = (np_bytepatch_t *)calloc(1, sizeof(*bp));
    if (!bp) return NULL;
    if (len && data) {
        bp->data = (unsigned char *)malloc(len);
        if (!bp->data) { free(bp); return NULL; }
        memcpy(bp->data, data, len);
        bp->len = bp->cap = len;
    }
    return bp;
}

void np_bytepatch_destroy(np_bytepatch_t *bp) {
    if (!bp) return;
    free(bp->data);
    snap_free(&bp->group_before);
    for (int i = 0; i < bp->undo_n; ++i) snap_free(&bp->undo_stack[i]);
    for (int i = 0; i < bp->redo_n; ++i) snap_free(&bp->redo_stack[i]);
    free(bp->undo_stack);
    free(bp->redo_stack);
    free(bp);
}

const unsigned char *np_bytepatch_data(const np_bytepatch_t *bp, size_t *out_len) {
    if (out_len) *out_len = bp ? bp->len : 0;
    return bp ? bp->data : NULL;
}

bool np_bytepatch_begin_group(np_bytepatch_t *bp) {
    if (!bp || bp->in_group) return false;
    if (!snap_copy(&bp->group_before, bp->data, bp->len)) return false;
    bp->in_group = true;
    bp->group_changed = false;
    return true;
}

static np_bp_result_t splice(np_bytepatch_t *bp, size_t at, size_t del_len,
                             const unsigned char *ins, size_t ins_len) {
    if (!bp->in_group) return NP_BP_ERR_ARG;
    if (at > bp->len || del_len > bp->len - at) return NP_BP_ERR_RANGE;
    size_t newlen = bp->len - del_len + ins_len;
    if (newlen > bp->cap) {
        size_t nc = bp->cap ? bp->cap : 64;
        while (nc < newlen) nc *= 2;
        unsigned char *p = (unsigned char *)realloc(bp->data, nc);
        if (!p) return NP_BP_ERR_ALLOC;
        bp->data = p; bp->cap = nc;
    }
    if (del_len != ins_len) {
        size_t tail = bp->len - at - del_len;
        memmove(bp->data + at + ins_len, bp->data + at + del_len, tail);
    }
    if (ins_len) memcpy(bp->data + at, ins, ins_len);
    bp->len = newlen;
    bp->group_changed = true;
    return NP_BP_OK;
}

np_bp_result_t np_bytepatch_replace(np_bytepatch_t *bp, size_t offset, size_t len,
                                    const unsigned char *bytes, size_t bytes_len) {
    if (!bp) return NP_BP_ERR_ARG;
    return splice(bp, offset, len, bytes, bytes_len);
}

np_bp_result_t np_bytepatch_insert(np_bytepatch_t *bp, size_t offset,
                                   const unsigned char *bytes, size_t bytes_len) {
    if (!bp) return NP_BP_ERR_ARG;
    return splice(bp, offset, 0, bytes, bytes_len);
}

np_bp_result_t np_bytepatch_delete(np_bytepatch_t *bp, size_t offset, size_t len) {
    if (!bp) return NP_BP_ERR_ARG;
    return splice(bp, offset, len, NULL, 0);
}

static bool push_snapshot(bp_snapshot_t **stack, int *n, int *cap,
                          const bp_snapshot_t *s) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        bp_snapshot_t *p = (bp_snapshot_t *)realloc(*stack, nc * sizeof(*p));
        if (!p) return false;
        *stack = p; *cap = nc;
    }
    if (!snap_copy(&(*stack)[*n], s->data, s->len)) return false;
    (*n)++;
    return true;
}

static bool pop_snapshot(bp_snapshot_t *stack, int *n, bp_snapshot_t *out) {
    if (*n <= 0) return false;
    *out = stack[*n - 1];
    (*n)--;
    return true;
}

bool np_bytepatch_end_group(np_bytepatch_t *bp) {
    if (!bp || !bp->in_group) return false;
    bp->in_group = false;
    if (!bp->group_changed) { snap_free(&bp->group_before); return false; }

    /* A new edit after undo invalidates the redo branch. */
    np_bytepatch_clear_redo(bp);

    bp_snapshot_t after;
    if (!snap_copy(&after, bp->data, bp->len)) return false;
    if (!push_snapshot(&bp->undo_stack, &bp->undo_n, &bp->undo_cap,
                       &bp->group_before)) { snap_free(&after); return false; }
    push_snapshot(&bp->undo_stack, &bp->undo_n, &bp->undo_cap, &after);
    /* cap history */
    while (bp->undo_n > NP_BP_MAX_HISTORY) {
        snap_free(&bp->undo_stack[0]);
        memmove(bp->undo_stack, bp->undo_stack + 1,
                (bp->undo_n - 1) * sizeof(*bp->undo_stack));
        bp->undo_n--;
    }
    snap_free(&bp->group_before);
    return true;
}

bool np_bytepatch_can_undo(const np_bytepatch_t *bp) { return bp && bp->undo_n >= 2; }
bool np_bytepatch_can_redo(const np_bytepatch_t *bp) { return bp && bp->redo_n >= 2; }

bool np_bytepatch_undo(np_bytepatch_t *bp) {
    if (!bp || !np_bytepatch_can_undo(bp)) return false;
    /* push current to redo (after-state is top of undo stack) */
    bp_snapshot_t cur;
    pop_snapshot(bp->undo_stack, &bp->undo_n, &cur); /* after  */
    push_snapshot(&bp->redo_stack, &bp->redo_n, &bp->redo_cap, &cur);
    bp_snapshot_t before;
    pop_snapshot(bp->undo_stack, &bp->undo_n, &before); /* before */
    push_snapshot(&bp->redo_stack, &bp->redo_n, &bp->redo_cap, &before);
    bool ok = snap_restore(&bp->data, &bp->len, &bp->cap, &before);
    snap_free(&cur); snap_free(&before);
    return ok;
}

bool np_bytepatch_redo(np_bytepatch_t *bp) {
    if (!bp || !np_bytepatch_can_redo(bp)) return false;
    bp_snapshot_t before;
    pop_snapshot(bp->redo_stack, &bp->redo_n, &before); /* before */
    push_snapshot(&bp->undo_stack, &bp->undo_n, &bp->undo_cap, &before);
    bp_snapshot_t after;
    pop_snapshot(bp->redo_stack, &bp->redo_n, &after);  /* after  */
    push_snapshot(&bp->undo_stack, &bp->undo_n, &bp->undo_cap, &after);
    bool ok = snap_restore(&bp->data, &bp->len, &bp->cap, &after);
    snap_free(&before); snap_free(&after);
    return ok;
}

void np_bytepatch_clear_redo(np_bytepatch_t *bp) {
    if (!bp) return;
    for (int i = 0; i < bp->redo_n; ++i) snap_free(&bp->redo_stack[i]);
    bp->redo_n = 0;
}
