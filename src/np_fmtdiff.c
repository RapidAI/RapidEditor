/* np_fmtdiff.c - F7 structured format / schema diff (see header). */
#include "np_fmtdiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_fd_op_name(np_fd_op_t op) {
    switch (op) {
        case NP_FD_ADD: return "ADD";
        case NP_FD_DEL: return "DEL";
        case NP_FD_MOD: return "MOD";
        default:        return "?";
    }
}
const char *np_fd_state_name(np_fd_state_t s) {
    switch (s) {
        case NP_FD_ACCEPTED: return "accepted";
        case NP_FD_REJECTED: return "rejected";
        default:             return "pending";
    }
}

static void scpy(char *d, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { d[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

void np_fd_set_init(np_fd_set_t *s) { if (s) memset(s, 0, sizeof *s); }
void np_fd_set_free(np_fd_set_t *s) {
    if (!s) return;
    free(s->items);
    memset(s, 0, sizeof *s);
}
bool np_fd_set_add(np_fd_set_t *s, const char *path, const char *text, const char *label) {
    if (!s || !path || !*path) return false;
    /* Reject duplicates: a structural path must be unique. */
    if (np_fd_set_find(s, path)) return false;
    if (s->count >= NP_FD_MAX_ELEMS) return false;
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        if (nc > NP_FD_MAX_ELEMS) nc = NP_FD_MAX_ELEMS;
        np_fd_elem_t *p = (np_fd_elem_t *)realloc(s->items, nc * sizeof(*p));
        if (!p) return false;
        s->items = p; s->cap = nc;
    }
    np_fd_elem_t *e = &s->items[s->count++];
    memset(e, 0, sizeof *e);
    scpy(e->path, sizeof e->path, path);
    scpy(e->text, sizeof e->text, text ? text : "");
    scpy(e->label, sizeof e->label, label ? label : "");
    return true;
}
const np_fd_elem_t *np_fd_set_find(const np_fd_set_t *s, const char *path) {
    if (!s || !path) return NULL;
    for (size_t i = 0; i < s->count; ++i)
        if (strcmp(s->items[i].path, path) == 0) return &s->items[i];
    return NULL;
}

void np_fd_result_init(np_fd_result_t *r) { if (r) memset(r, 0, sizeof *r); }
void np_fd_result_free(np_fd_result_t *r) {
    if (!r) return;
    free(r->items);
    memset(r, 0, sizeof *r);
}

static int elem_cmp(const void *pa, const void *pb) {
    const np_fd_elem_t *a = (const np_fd_elem_t *)pa;
    const np_fd_elem_t *b = (const np_fd_elem_t *)pb;
    return strcmp(a->path, b->path);
}

static bool push_change(np_fd_result_t *out, np_fd_op_t op,
                        const np_fd_elem_t *a, const np_fd_elem_t *b) {
    if (out->count >= NP_FD_MAX_CHANGES) return false;
    if (out->count == out->cap) {
        size_t nc = out->cap ? out->cap * 2 : 32;
        if (nc > NP_FD_MAX_CHANGES) nc = NP_FD_MAX_CHANGES;
        np_fd_change_t *p = (np_fd_change_t *)realloc(out->items, nc * sizeof(*p));
        if (!p) return false;
        out->items = p; out->cap = nc;
    }
    np_fd_change_t *c = &out->items[out->count++];
    memset(c, 0, sizeof *c);
    c->op = op; c->state = NP_FD_PENDING;
    const np_fd_elem_t *src = a ? a : b;
    scpy(c->path, sizeof c->path, src->path);
    if (a) { scpy(c->before_text, sizeof c->before_text, a->text);
             scpy(c->before_label, sizeof c->before_label, a->label); }
    if (b) { scpy(c->after_text, sizeof c->after_text, b->text);
             scpy(c->after_label, sizeof c->after_label, b->label); }
    return true;
}

bool np_fd_diff(const np_fd_set_t *a, const np_fd_set_t *b, np_fd_result_t *out) {
    if (!a || !b || !out) return false;
    np_fd_result_free(out);
    np_fd_result_init(out);

    /* Copy + sort both sets to get a stable merge order. */
    np_fd_elem_t *sa = NULL, *sb = NULL;
    if (a->count) {
        sa = (np_fd_elem_t *)malloc(a->count * sizeof(*sa));
        if (!sa) { scpy(out->err, sizeof out->err, "内存不足"); return false; }
        memcpy(sa, a->items, a->count * sizeof(*sa));
        qsort(sa, a->count, sizeof(*sa), elem_cmp);
    }
    if (b->count) {
        sb = (np_fd_elem_t *)malloc(b->count * sizeof(*sb));
        if (!sb) { free(sa); scpy(out->err, sizeof out->err, "内存不足"); return false; }
        memcpy(sb, b->items, b->count * sizeof(*sb));
        qsort(sb, b->count, sizeof(*sb), elem_cmp);
    }

    size_t i = 0, j = 0;
    bool ok = true;
    while (i < a->count || j < b->count) {
        int cmp;
        if (i >= a->count) cmp = 1;
        else if (j >= b->count) cmp = -1;
        else cmp = strcmp(sa[i].path, sb[j].path);
        if (cmp == 0) {
            if (strcmp(sa[i].text, sb[j].text) != 0 ||
                strcmp(sa[i].label, sb[j].label) != 0) {
                if (!push_change(out, NP_FD_MOD, &sa[i], &sb[j])) { ok = false; break; }
            }
            i++; j++;
        } else if (cmp < 0) {
            if (!push_change(out, NP_FD_DEL, &sa[i], NULL)) { ok = false; break; }
            i++;
        } else {
            if (!push_change(out, NP_FD_ADD, NULL, &sb[j])) { ok = false; break; }
            j++;
        }
    }
    free(sa); free(sb);
    if (!ok) scpy(out->err, sizeof out->err, "变更数超限或内存不足");
    return ok;
}

bool np_fd_accept(np_fd_result_t *r, size_t idx) {
    if (!r || idx >= r->count) return false;
    r->items[idx].state = NP_FD_ACCEPTED;
    return true;
}
bool np_fd_reject(np_fd_result_t *r, size_t idx) {
    if (!r || idx >= r->count) return false;
    r->items[idx].state = NP_FD_REJECTED;
    return true;
}
size_t np_fd_count_by_state(const np_fd_result_t *r, np_fd_state_t s) {
    if (!r) return 0;
    size_t n = 0;
    for (size_t i = 0; i < r->count; ++i) if (r->items[i].state == s) ++n;
    return n;
}

size_t np_fd_render_accepted(const np_fd_result_t *r, char *out, size_t cap) {
    if (!r || !out || cap == 0) return 0;
    out[0] = 0;
    size_t at = 0;
    for (size_t i = 0; i < r->count; ++i) {
        const np_fd_change_t *c = &r->items[i];
        if (c->state != NP_FD_ACCEPTED) continue;
        int n = snprintf(out + at, cap - at, "%s\t%s\t%s\n",
                         np_fd_op_name(c->op), c->path,
                         c->after_text[0] ? c->after_text : c->before_text);
        if (n < 0 || (size_t)n >= cap - at) break;
        at += (size_t)n;
    }
    return at;
}
