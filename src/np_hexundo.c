/* np_hexundo.c - multi-level undo for manual hex editing (see header). */
#include "np_hexundo.h"
#include "np_bytepatch.h"

#include <stdlib.h>
#include <string.h>

struct np_hexundo {
    np_bytepatch_t *bp;      /* shadow state + undo/redo stacks */
    double          now;     /* monotonic seconds advanced by tick() */
    double          last_edit;
    bool            in_session;
    bool            have_last;
    unsigned char  *last;    /* last observed buffer, for change detection */
    size_t          last_len;
};

np_hexundo_t *np_hexundo_create(const void *data, size_t len) {
    np_hexundo_t *h = (np_hexundo_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->bp = np_bytepatch_create(data ? data : "", data ? len : 0);
    if (!h->bp) { free(h); return NULL; }
    if (len) {
        h->last = (unsigned char *)malloc(len);
        if (!h->last) { np_bytepatch_destroy(h->bp); free(h); return NULL; }
        memcpy(h->last, data, len);
    }
    h->last_len = len;
    h->have_last = true;
    h->last_edit = -1e9;    /* long ago */
    return h;
}

void np_hexundo_destroy(np_hexundo_t *h) {
    if (!h) return;
    if (h->in_session) np_bytepatch_end_group(h->bp);
    np_bytepatch_destroy(h->bp);
    free(h->last);
    free(h);
}

void np_hexundo_tick(np_hexundo_t *h, double dt) {
    if (!h) return;
    if (dt > 0.0 && dt < 1.0) h->now += dt;   /* clamp huge frame stalls */
}

static bool differs(const np_hexundo_t *h, const void *cur, size_t len) {
    if (!h->have_last) return true;
    if (h->last_len != len) return true;
    return len && memcmp(h->last, cur, len) != 0;
}

void np_hexundo_observe(np_hexundo_t *h, const void *cur, size_t len) {
    if (!h || !cur) return;
    if (!differs(h, cur, len)) return;

    size_t shadow_len = 0;
    np_bytepatch_data(h->bp, &shadow_len);   /* current shadow length */

    bool new_session = !h->in_session ||
                       (h->now - h->last_edit) > NP_HEXUNDO_WINDOW;
    if (new_session) {
        if (h->in_session) np_bytepatch_end_group(h->bp);
        np_bytepatch_clear_redo(h->bp);
        if (!np_bytepatch_begin_group(h->bp)) return;
        h->in_session = true;
    }

    /* Replace the whole shadow from the observed buffer. np_bytepatch allows
     * the length to change, so undo restores exact bytes AND length (H1's
     * contract). External replacements (file reload / mode switch) are the
     * caller's responsibility to signal via np_hexundo_reset(). */
    np_bytepatch_replace(h->bp, 0, shadow_len,
                         (const unsigned char *)cur, len);

    h->last_edit = h->now;
    if (h->last_len < len) {
        unsigned char *p = (unsigned char *)realloc(h->last, len ? len : 1);
        if (p) { h->last = p; h->last_len = len; }
    }
    if (h->last) memcpy(h->last, cur, len);
    h->have_last = true;
}

void np_hexundo_flush(np_hexundo_t *h) {
    if (!h || !h->in_session) return;
    np_bytepatch_end_group(h->bp);
    h->in_session = false;
}

bool np_hexundo_can_undo(const np_hexundo_t *h) {
    return h && (h->in_session || np_bytepatch_can_undo(h->bp));
}
bool np_hexundo_can_redo(const np_hexundo_t *h) {
    return h && !h->in_session && np_bytepatch_can_redo(h->bp);
}

static bool sync_last_from_shadow(np_hexundo_t *h) {
    size_t len = 0;
    const unsigned char *p = np_bytepatch_data(h->bp, &len);
    if (h->last_len < len) {
        unsigned char *q = (unsigned char *)realloc(h->last, len ? len : 1);
        if (!q) return false;
        h->last = q;
        h->last_len = len;
    }
    if (len && h->last) memcpy(h->last, p, len);
    h->have_last = true;
    return true;
}

bool np_hexundo_undo(np_hexundo_t *h) {
    if (!h) return false;
    if (h->in_session) {
        np_bytepatch_end_group(h->bp);   /* close the in-flight session first */
        h->in_session = false;
    }
    if (!np_bytepatch_undo(h->bp)) return false;
    return sync_last_from_shadow(h);
}

bool np_hexundo_redo(np_hexundo_t *h) {
    if (!h || h->in_session) return false;
    if (!np_bytepatch_redo(h->bp)) return false;
    return sync_last_from_shadow(h);
}

const unsigned char *np_hexundo_data(const np_hexundo_t *h, size_t *out_len) {
    return np_bytepatch_data(h ? h->bp : NULL, out_len);
}

void np_hexundo_reset(np_hexundo_t *h, const void *data, size_t len) {
    if (!h) return;
    np_bytepatch_destroy(h->bp);
    h->bp = np_bytepatch_create(data ? data : "", data ? len : 0);
    free(h->last);
    h->last = NULL;
    h->last_len = 0;
    h->have_last = false;
    h->in_session = false;
    h->last_edit = -1e9;
    if (h->bp && len) {
        h->last = (unsigned char *)malloc(len);
        if (h->last) {
            memcpy(h->last, data, len);
            h->last_len = len;
            h->have_last = true;
        }
    }
}
