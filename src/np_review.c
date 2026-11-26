/* np_review.c — M2/B3: 统一 Patch Review 会话模型. */
#include "np_review.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_review_state_name(np_review_state_t s) {
    switch (s) {
    case NP_REVIEW_PENDING: return "pending";
    case NP_REVIEW_ACCEPTED: return "accepted";
    default: return "rejected";
    }
}

/* 与 hunks 平行的数据槽：字节线存新字节，文本线存新文本副本 */
typedef struct {
    size_t off, len;            /* 字节线：[off, off+len) */
    unsigned char *bytes;       /* 字节线新内容；NULL=delete */
    size_t bytes_len;
    char   *text;               /* 文本线新内容副本；NULL=delete */
    size_t  text_len;
} np_review_data_t;

struct np_review {
    np_track_t track;
    uint64_t base_revision;
    bool gate_allowed;
    char gate_reason[160];

    np_review_hunk_t *hunks;
    np_review_data_t *data;
    size_t count, cap;
};

np_review_t *np_review_create(np_track_t track, uint64_t base_revision) {
    np_review_t *rv = (np_review_t *)calloc(1, sizeof(*rv));
    if (!rv) return NULL;
    rv->track = track;
    rv->base_revision = base_revision;
    rv->gate_allowed = true;
    snprintf(rv->gate_reason, sizeof(rv->gate_reason), "%s", "no gate set");
    return rv;
}

void np_review_destroy(np_review_t *rv) {
    if (!rv) return;
    for (size_t i = 0; i < rv->count; ++i) {
        free(rv->data[i].bytes);
        free(rv->data[i].text);
    }
    free(rv->data);
    free(rv->hunks);
    free(rv);
}

void np_review_set_gate(np_review_t *rv, bool allowed, const char *reason) {
    if (!rv) return;
    rv->gate_allowed = allowed;
    snprintf(rv->gate_reason, sizeof(rv->gate_reason), "%s",
             reason ? reason : "");
}

static bool ensure_cap(np_review_t *rv, size_t need) {
    if (need <= rv->cap) return true;
    size_t ncap = rv->cap ? rv->cap * 2 : 8;
    while (ncap < need) ncap *= 2;
    np_review_hunk_t *nh = (np_review_hunk_t *)realloc(rv->hunks, ncap * sizeof(*nh));
    if (!nh) return false;
    rv->hunks = nh;
    np_review_data_t *nd = (np_review_data_t *)realloc(rv->data, ncap * sizeof(*nd));
    if (!nd) return false;
    rv->data = nd;
    for (size_t i = rv->cap; i < ncap; ++i) {
        memset(&rv->hunks[i], 0, sizeof(rv->hunks[i]));
        memset(&rv->data[i], 0, sizeof(rv->data[i]));
    }
    rv->cap = ncap;
    return true;
}

static const char *op_name(np_patch_op_t op) {
    switch (op) {
    case NP_OP_INSERT: return "insert";
    case NP_OP_DELETE: return "delete";
    default: return "replace";
    }
}

size_t np_review_import_text(np_review_t *rv, const np_patch_set_t *ps) {
    if (!rv || !ps || rv->track != NP_TRACK_TEXT) return 0;
    size_t n = np_patch_set_count(ps);
    size_t imported = 0;
    for (size_t i = 0; i < n; ++i) {
        const np_patch_hunk_t *h = np_patch_set_hunk(ps, i);
        if (!h || h->track != NP_TRACK_TEXT) continue;
        if (!ensure_cap(rv, rv->count + 1)) break;

        np_review_hunk_t *rh = &rv->hunks[rv->count];
        np_review_data_t *rd = &rv->data[rv->count];
        memset(rh, 0, sizeof(*rh));
        rh->state = NP_REVIEW_PENDING;
        rh->line_first = h->u.text.line_first;
        rh->line_last = h->u.text.line_last;
        rh->op = h->op;

        /* 新文本副本 */
        if (h->op != NP_OP_DELETE && h->u.text.text && h->u.text.text_len) {
            rd->text = (char *)malloc(h->u.text.text_len + 1);
            if (!rd->text) break;
            memcpy(rd->text, h->u.text.text, h->u.text.text_len);
            rd->text[h->u.text.text_len] = '\0';
            rd->text_len = h->u.text.text_len;
        }

        /* 摘要（首行） */
        if (rh->op == NP_OP_DELETE) {
            snprintf(rh->preview, sizeof(rh->preview), "%s", "(delete lines)");
        } else if (rd->text) {
            size_t first_line = 0;
            while (first_line < rd->text_len && rd->text[first_line] != '\n')
                first_line++;
            if (first_line > sizeof(rh->preview) - 24)
                first_line = sizeof(rh->preview) - 24;
            snprintf(rh->preview, sizeof(rh->preview), "%s: %.*s%s",
                     op_name(rh->op), (int)first_line, rd->text,
                     first_line < rd->text_len ? "..." : "");
        } else {
            snprintf(rh->preview, sizeof(rh->preview), "%s", op_name(rh->op));
        }
        rv->count++;
        imported++;
    }
    return imported;
}

size_t np_review_import_byte(np_review_t *rv, const np_patch_set_t *ps,
                             const np_protected_t *prot, size_t nprot) {
    if (!rv || !ps || rv->track != NP_TRACK_BINARY) return 0;
    size_t n = np_patch_set_count(ps);
    size_t imported = 0;
    for (size_t i = 0; i < n; ++i) {
        const np_patch_hunk_t *h = np_patch_set_hunk(ps, i);
        if (!h || h->track != NP_TRACK_BINARY) continue;
        const unsigned char *bytes =
            (h->op != NP_OP_DELETE && h->u.byte.bytes) ? h->u.byte.bytes : NULL;
        size_t blen = bytes ? h->u.byte.bytes_len : 0;
        if (np_review_add_byte(rv, h->u.byte.offset, h->u.byte.len,
                               bytes, blen, prot, nprot))
            imported++;
    }
    return imported;
}

bool np_review_add_byte(np_review_t *rv, size_t off, size_t len,
                        const unsigned char *bytes, size_t bytes_len,
                        const np_protected_t *prot, size_t nprot) {
    if (!rv || rv->track != NP_TRACK_BINARY) return false;
    if (!ensure_cap(rv, rv->count + 1)) return false;

    np_review_hunk_t *rh = &rv->hunks[rv->count];
    np_review_data_t *rd = &rv->data[rv->count];
    memset(rh, 0, sizeof(*rh));
    rh->state = NP_REVIEW_PENDING;
    rh->byte_off = off;
    rh->byte_len = len;
    rh->new_len = bytes ? bytes_len : 0;
    if (bytes && bytes_len)
        snprintf(rh->preview, sizeof(rh->preview), "replace 0x%zX [%zu] -> %zu bytes",
                 off, len, bytes_len);
    else if (len)
        snprintf(rh->preview, sizeof(rh->preview), "delete 0x%zX [%zu]", off, len);
    else
        snprintf(rh->preview, sizeof(rh->preview), "insert 0x%zX %zu bytes",
                 off, bytes_len);

    /* H2 保护区冲突预检 */
    size_t chg_lo = off, chg_hi = off + (len > 0 ? len : 0);
    if (bytes && bytes_len > len) chg_hi = off + bytes_len;
    for (size_t i = 0; i < nprot; ++i) {
        size_t p_lo = prot[i].offset;
        size_t p_hi = prot[i].offset + prot[i].len;
        if (chg_lo < p_hi && p_lo < chg_hi) {
            rh->guard_blocked = true;
            snprintf(rh->guard_reason, sizeof(rh->guard_reason),
                     "touches protected region '%s'", prot[i].name);
            break;
        }
    }

    if (bytes && bytes_len) {
        unsigned char *nb = (unsigned char *)malloc(bytes_len);
        if (!nb) return false;
        memcpy(nb, bytes, bytes_len);
        rd->bytes = nb;
        rd->bytes_len = bytes_len;
    }
    rd->off = off;
    rd->len = len;
    rv->count++;
    return true;
}

bool np_review_accept(np_review_t *rv, size_t idx) {
    if (!rv || idx >= rv->count) return false;
    rv->hunks[idx].state = NP_REVIEW_ACCEPTED;
    return true;
}

bool np_review_reject(np_review_t *rv, size_t idx) {
    if (!rv || idx >= rv->count) return false;
    rv->hunks[idx].state = NP_REVIEW_REJECTED;
    return true;
}

size_t np_review_count(const np_review_t *rv) { return rv ? rv->count : 0; }

const np_review_hunk_t *np_review_hunk(const np_review_t *rv, size_t idx) {
    if (!rv || idx >= rv->count) return NULL;
    return &rv->hunks[idx];
}

long np_review_byte_delta(const np_review_t *rv) {
    if (!rv) return 0;
    long delta = 0;
    for (size_t i = 0; i < rv->count; ++i) {
        if (rv->hunks[i].state != NP_REVIEW_ACCEPTED) continue;
        delta += (long)rv->data[i].bytes_len - (long)rv->data[i].len;
    }
    return delta;
}

np_patch_result_t np_review_commit_text(np_review_t *rv, np_patch_buffer_t *b,
                                        char *why, size_t why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (!rv || !b || rv->track != NP_TRACK_TEXT) return NP_PATCH_ERR_ARG;
    if (!rv->gate_allowed) {
        if (why && why_cap) snprintf(why, why_cap, "gate: %s", rv->gate_reason);
        return NP_PATCH_ERR_ARG;
    }
    if (b->revision != rv->base_revision) {
        if (why && why_cap) snprintf(why, why_cap, "stale base revision");
        return NP_PATCH_ERR_REVISION;
    }

    np_patch_set_t *ps = np_patch_set_create(rv->base_revision);
    if (!ps) return NP_PATCH_ERR_ALLOC;

    np_patch_result_t ret = NP_PATCH_OK;
    for (size_t i = 0; i < rv->count; ++i) {
        if (rv->hunks[i].state != NP_REVIEW_ACCEPTED) continue;
        const np_review_hunk_t *rh = &rv->hunks[i];
        const np_review_data_t *rd = &rv->data[i];
        ret = np_patch_add_text(ps, rh->op, rh->line_first, rh->line_last,
                                rd->text, rd->text ? rd->text_len : 0);
        if (ret != NP_PATCH_OK) break;
    }
    if (ret == NP_PATCH_OK) {
        if (np_patch_set_count(ps) == 0) {
            if (why && why_cap) snprintf(why, why_cap, "nothing accepted");
            ret = NP_PATCH_ERR_ARG;
        } else {
            ret = np_patch_apply(ps, NP_TRACK_TEXT, b);
            if (ret != NP_PATCH_OK && why && why_cap)
                snprintf(why, why_cap, "apply failed");
        }
    } else if (why && why_cap) {
        snprintf(why, why_cap, "rebuild patch set failed");
    }
    np_patch_set_destroy(ps);
    return ret;
}

np_bp_result_t np_review_commit_byte(np_review_t *rv, np_bytepatch_t *bp,
                                     char *why, size_t why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (!rv || !bp || rv->track != NP_TRACK_BINARY) return NP_BP_ERR_ARG;
    if (!rv->gate_allowed) {
        if (why && why_cap) snprintf(why, why_cap, "gate: %s", rv->gate_reason);
        return NP_BP_ERR_ARG;
    }
    /* H7: accepted hunks may be variable-length (insert/delete). Apply them
     * in DESCENDING offset order so an earlier splice cannot shift the offsets
     * of hunks still pending (forward order corrupts multi-hunk diffs). */
    size_t nacc = 0;
    for (size_t i = 0; i < rv->count; ++i) {
        if (rv->hunks[i].state != NP_REVIEW_ACCEPTED) continue;
        if (rv->hunks[i].guard_blocked) {
            if (why && why_cap)
                snprintf(why, why_cap, "hunk %zu blocked: %s",
                         i, rv->hunks[i].guard_reason);
            return NP_BP_ERR_ARG;
        }
        nacc++;
    }
    if (!nacc) {
        if (why && why_cap) snprintf(why, why_cap, "nothing accepted");
        return NP_BP_ERR_ARG;
    }
    size_t *order = (size_t *)malloc(nacc * sizeof(*order));
    if (!order) return NP_BP_ERR_ALLOC;
    size_t m = 0;
    for (size_t i = 0; i < rv->count; ++i)
        if (rv->hunks[i].state == NP_REVIEW_ACCEPTED) order[m++] = i;
    /* stable insertion sort, descending by offset */
    for (size_t a = 1; a < m; ++a) {
        size_t cur = order[a], b = a;
        while (b > 0 && rv->data[order[b - 1]].off < rv->data[cur].off) {
            order[b] = order[b - 1]; b--;
        }
        order[b] = cur;
    }
    if (!np_bytepatch_begin_group(bp)) { free(order); return NP_BP_ERR_ALLOC; }

    np_bp_result_t ret = NP_BP_OK;
    for (size_t k = 0; k < m; ++k) {
        const size_t i = order[k];
        const size_t off = rv->data[i].off, len = rv->data[i].len;
        const unsigned char *bytes = rv->data[i].bytes;
        const size_t blen = rv->data[i].bytes_len;
        np_bp_result_t r;
        if (bytes && blen)      r = np_bytepatch_replace(bp, off, len, bytes, blen);
        else if (len)           r = np_bytepatch_delete(bp, off, len);
        else                    r = np_bytepatch_insert(bp, off, bytes, blen);
        if (r != NP_BP_OK) {
            if (why && why_cap)
                snprintf(why, why_cap, "hunk %zu failed: %s", i, np_bp_result_name(r));
            np_bytepatch_undo(bp);
            ret = r;
            break;
        }
    }
    free(order);
    if (ret == NP_BP_OK) np_bytepatch_end_group(bp);
    return ret;
}
