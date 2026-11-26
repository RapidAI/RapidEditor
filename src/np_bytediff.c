/* np_bytediff.c - H7 byte-level diff (see np_bytediff.h). */
#include "np_bytediff.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Edit-distance / hunk caps. Beyond these we degrade to one coarse REPLACE
 * hunk covering the whole differing middle region (fuzzy=true) -- still fully
 * replayable, just not fine-grained. */
#define NP_BD_MAX_D      512
#define NP_BD_MAX_HUNKS  512

const char *np_bd_kind_name(np_bd_kind_t k) {
    switch (k) {
    case NP_BD_REPLACE: return "replace";
    case NP_BD_INSERT:  return "insert";
    case NP_BD_DELETE:  return "delete";
    default: return "?";
    }
}

void np_bytediff_free(np_bd_result_t *out) {
    if (!out) return;
    free(out->hunks);
    out->hunks = NULL;
    out->nhunks = 0;
}

/* Append one hunk with bounded before/after preview bytes. */
static bool bd_push(np_bd_result_t *out, np_bd_kind_t kind,
                    const unsigned char *A, size_t boff, size_t blen,
                    const unsigned char *B, size_t aoff, size_t alen) {
    if (!out || out->nhunks >= NP_BD_MAX_HUNKS) return false;
    np_bd_hunk_t *h = &out->hunks[out->nhunks++];
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    h->before_off = boff; h->before_len = blen;
    h->after_off  = aoff; h->after_len  = alen;
    h->before_prev_n = blen < NP_BD_PREVIEW ? blen : NP_BD_PREVIEW;
    h->after_prev_n  = alen < NP_BD_PREVIEW ? alen : NP_BD_PREVIEW;
    if (h->before_prev_n && A) memcpy(h->before_bytes, A + boff, h->before_prev_n);
    if (h->after_prev_n  && B) memcpy(h->after_bytes,  B + aoff, h->after_prev_n);
    return true;
}

/* Replay-based verification: walk before/after in lockstep; each hunk consumes
 * [before_off, before_off+before_len) from before and emits [after_off,
 * after_off+after_len) from after. Everything else must be byte-equal. */
bool np_bytediff_verify(const void *before, size_t blen,
                        const np_bd_result_t *res,
                        const void *after, size_t alen) {
    if (!res || res->after_len != alen) return false;
    const unsigned char *A = (const unsigned char *)before;
    const unsigned char *B = (const unsigned char *)after;
    size_t pos = 0, ab = 0;
    for (size_t i = 0; i < res->nhunks; ++i) {
        const np_bd_hunk_t *h = &res->hunks[i];
        if (h->before_off < pos || h->before_off > blen) return false;
        size_t copy = h->before_off - pos;
        if (copy > alen - ab) return false;
        if (copy && (!A || !B || memcmp(A + pos, B + ab, copy) != 0)) return false;
        ab += copy;
        if (h->kind != NP_BD_DELETE) {
            if (h->after_len > alen - ab) return false;
            if (h->after_off > alen || h->after_len > alen - h->after_off) return false;
            if (h->after_len && (!B || memcmp(B + h->after_off, B + ab, h->after_len) != 0))
                return false;
            ab += h->after_len;
        } else if (h->after_len != 0) {
            return false;
        }
        pos = h->before_off + h->before_len;
        if (pos > blen) return false;
    }
    size_t tail = blen - pos;
    if (tail > alen - ab) return false;
    if (tail && (!A || !B || memcmp(A + pos, B + ab, tail) != 0)) return false;
    ab += tail;
    return ab == alen;
}

bool np_bytediff_apply(const void *before, size_t blen,
                       const void *after, const np_bd_result_t *res,
                       unsigned char *out, size_t out_cap, size_t *out_len) {
    if (!res || !out || out_cap < res->after_len) return false;
    const unsigned char *A = (const unsigned char *)before;
    const unsigned char *B = (const unsigned char *)after;
    size_t o = 0, pos = 0;
    for (size_t i = 0; i < res->nhunks; ++i) {
        const np_bd_hunk_t *h = &res->hunks[i];
        if (h->before_off < pos || h->before_off > blen) return false;
        size_t copy = h->before_off - pos;
        if (copy > out_cap - o) return false;
        if (copy) {
            if (!A) return false;
            memcpy(out + o, A + pos, copy); o += copy;
        }
        pos = h->before_off + h->before_len;
        if (pos > blen) return false;
        if (h->kind != NP_BD_DELETE) {
            if (h->after_len > out_cap - o) return false;
            if (h->after_len) {
                if (!B || h->after_off > res->after_len ||
                    h->after_len > res->after_len - h->after_off) return false;
                memcpy(out + o, B + h->after_off, h->after_len); o += h->after_len;
            }
        }
    }
    size_t tail = blen - pos;
    if (tail > out_cap - o) return false;
    if (tail) {
        if (!A) return false;
        memcpy(out + o, A + pos, tail); o += tail;
    }
    if (out_len) *out_len = o;
    return o == res->after_len;
}

/* ---- Myers O(ND) on the differing middle region ------------------------ */

typedef enum { BD_EQ = 0, BD_INS, BD_DEL } bd_tok_kind_t;

typedef struct {
    bd_tok_kind_t kind;
    size_t        bx, by;   /* path point (before consumed, after consumed) */
    size_t        eqn;      /* BD_EQ: number of matching bytes in this run */
} bd_token_t;

static void bd_cleanup(int *V, int **trace, int maxd) {
    if (trace) {
        for (int d = 0; d <= maxd; ++d) free(trace[d]);
        free(trace);
    }
    free(V);
}

/* Fine diff via Myers shortest edit script. Returns false on D>cap or OOM;
 * caller then degrades to fuzzy. */
static bool bd_myers(np_bd_result_t *out, const unsigned char *A,
                     const unsigned char *B, size_t nb, size_t na, size_t P) {
    size_t span = nb + na;
    int maxd = (int)(span < (size_t)NP_BD_MAX_D ? span : (size_t)NP_BD_MAX_D);
    int vsize = 2 * maxd + 3;
    int off   = maxd + 1;
    int *V = (int *)calloc((size_t)vsize, sizeof(int));
    int **trace = (int **)calloc((size_t)maxd + 1, sizeof(int *));
    if (!V || !trace) { bd_cleanup(V, trace, maxd); return false; }

    int found = -1;
    for (int d = 0; d <= maxd; ++d) {
        trace[d] = (int *)malloc((size_t)(2 * d + 1) * sizeof(int));
        if (!trace[d]) { bd_cleanup(V, trace, maxd); return false; }
        for (int k = -d; k <= d; ++k) trace[d][k + d] = V[k + off];
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && V[k - 1 + off] < V[k + 1 + off]))
                x = V[k + 1 + off];                 /* insertion (down) */
            else
                x = V[k - 1 + off] + 1;             /* deletion (right)  */
            int y = x - k;
            while ((size_t)x < nb && (size_t)y < na &&
                   A[P + (size_t)x] == B[P + (size_t)y]) { x++; y++; }
            V[k + off] = x;
            if ((size_t)x >= nb && (size_t)y >= na) { found = d; break; }
        }
        if (found >= 0) break;
    }
    if (found < 0) { bd_cleanup(V, trace, maxd); return false; }

    size_t tcap = (size_t)found * 2 + 2;
    bd_token_t *tok = (bd_token_t *)calloc(tcap, sizeof(bd_token_t));
    if (!tok) { bd_cleanup(V, trace, maxd); return false; }
    size_t tn = 0;

    int x = (int)nb, y = (int)na;
    for (int d = found; d >= 1; --d) {
        int *Vp = trace[d];                 /* V as of the end of round d-1 */
        int k = x - y;
        int pk;
        if (k == -d || (k != d && Vp[k - 1 + d] < Vp[k + 1 + d])) pk = k + 1;
        else pk = k - 1;
        int px = Vp[pk + d];
        int py = px - pk;
        int eqn = 0;
        while (x > px && y > py) { x--; y--; eqn++; }
        if (eqn && tn < tcap) {
            tok[tn].kind = BD_EQ; tok[tn].eqn = (size_t)eqn; tn++;
        }
        if (tn >= tcap) { free(tok); bd_cleanup(V, trace, maxd); return false; }
        if (x == px) {                      /* insertion of B[y-1] */
            tok[tn].kind = BD_INS;
            tok[tn].bx = (size_t)x; tok[tn].by = (size_t)(y - 1); tn++;
            y--;
        } else {                            /* deletion of A[x-1] */
            tok[tn].kind = BD_DEL;
            tok[tn].bx = (size_t)(x - 1); tok[tn].by = (size_t)y; tn++;
            x--;
        }
    }
    bd_cleanup(V, trace, maxd);

    /* reverse into forward order */
    for (size_t i = 0; i < tn / 2; ++i) {
        bd_token_t t = tok[i];
        tok[i] = tok[tn - 1 - i];
        tok[tn - 1 - i] = t;
    }

    bool seg = false;
    size_t sb = 0, sa = 0, bl = 0, al = 0;
    bool ok = true;
    for (size_t i = 0; i < tn; ++i) {
        bd_token_t *t = &tok[i];
        if (t->kind == BD_EQ) {
            if (t->eqn > 0 && seg) {
                np_bd_kind_t k = bl && al ? NP_BD_REPLACE :
                                 (bl ? NP_BD_DELETE : NP_BD_INSERT);
                if (!bd_push(out, k, A, P + sb, bl, B, P + sa, al)) { ok = false; break; }
                seg = false; bl = al = 0;
            }
        } else {
            if (!seg) { seg = true; sb = t->bx; sa = t->by; bl = 0; al = 0; }
            if (t->kind == BD_INS) al++; else bl++;
        }
    }
    if (ok && seg) {
        np_bd_kind_t k = bl && al ? NP_BD_REPLACE :
                         (bl ? NP_BD_DELETE : NP_BD_INSERT);
        ok = bd_push(out, k, A, P + sb, bl, B, P + sa, al);
    }
    free(tok);
    return ok;
}

/* Coarse single-hunk fallback: always replayable. */
static void bd_fuzzy(np_bd_result_t *out, const unsigned char *A,
                     const unsigned char *B, size_t P, size_t mb, size_t ma) {
    out->nhunks = 0;
    out->fuzzy = true;
    np_bd_kind_t k = (mb && ma) ? NP_BD_REPLACE :
                     (ma ? NP_BD_INSERT : NP_BD_DELETE);
    if (!(mb || ma)) return;
    (void)bd_push(out, k, A, P, mb, B, P, ma);
}

bool np_bytediff_compute(const void *before, size_t blen,
                         const void *after, size_t alen,
                         np_bd_result_t *out) {
    if (!out || (blen && !before) || (alen && !after)) return false;
    memset(out, 0, sizeof(*out));
    out->before_len = blen;
    out->after_len  = alen;
    out->crc_before = np_crc32(blen ? before : "", blen);
    out->crc_after  = np_crc32(alen ? after  : "", alen);
    out->hunks = (np_bd_hunk_t *)calloc(NP_BD_MAX_HUNKS, sizeof(np_bd_hunk_t));
    if (!out->hunks) return false;

    const unsigned char *A = (const unsigned char *)before;
    const unsigned char *B = (const unsigned char *)after;
    if (blen == alen && blen && memcmp(A, B, blen) == 0) return true; /* identical */

    size_t P = 0;
    while (P < blen && P < alen && A[P] == B[P]) P++;
    size_t S = 0;
    while (S < blen - P && S < alen - P &&
           A[blen - 1 - S] == B[alen - 1 - S]) S++;
    size_t mb = blen - P - S, ma = alen - P - S;
    if (!mb && !ma) return true;

    bool ok = bd_myers(out, A, B, mb, ma, P);
    if (!ok || !np_bytediff_verify(before, blen, out, after, alen)) {
        bd_fuzzy(out, A, B, P, mb, ma);
        if (!np_bytediff_verify(before, blen, out, after, alen)) {
            np_bytediff_free(out);
            return false;
        }
    }
    return true;
}
