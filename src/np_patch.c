/* np_patch.c - A3 three-track patch protocol (see np_patch.h). */
#include "np_patch.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct np_patch_set {
    uint64_t          base_revision;
    np_patch_hunk_t  *hunks;
    size_t            count;
    size_t            cap;

    /* copy-on-write undo snapshot, captured on the last successful apply */
    unsigned char    *undo_data;
    size_t            undo_len;
    uint64_t          undo_revision;
    bool              has_undo;
};

const char *np_patch_result_name(np_patch_result_t r) {
    switch (r) {
        case NP_PATCH_OK:          return "ok";
        case NP_PATCH_ERR_ARG:     return "arg";
        case NP_PATCH_ERR_REVISION:return "stale-revision";
        case NP_PATCH_ERR_RANGE:   return "range";
        case NP_PATCH_ERR_TRACK:   return "track";
        case NP_PATCH_ERR_NEEDS_F8:return "needs-f8-writer";
        case NP_PATCH_ERR_ALLOC:   return "alloc";
        default:                   return "unknown";
    }
}

/* --- buffer ---------------------------------------------------------- */
void np_patch_buffer_init(np_patch_buffer_t *b) {
    if (!b) return;
    b->data = NULL; b->len = 0; b->cap = 0; b->revision = 0;
}

void np_patch_buffer_free(np_patch_buffer_t *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
}

bool np_patch_buffer_set(np_patch_buffer_t *b, const void *data, size_t len) {
    if (!b) return false;
    if (len > b->cap) {
        unsigned char *p = (unsigned char *)realloc(b->data, len ? len : 1);
        if (!p) return false;
        b->data = p;
        b->cap = len;
    }
    if (len) memcpy(b->data, data, len);
    b->len = len;
    return true;
}

size_t np_patch_line_count(const np_patch_buffer_t *b) {
    if (!b || !b->data || b->len == 0) return 0;
    size_t n = 1;
    for (size_t i = 0; i < b->len; ++i) if (b->data[i] == '\n') n++;
    /* A trailing newline does not create an extra empty line. */
    if (b->data[b->len - 1] == '\n') n--;
    return n;
}

/* 0-based line addressing (patch API semantics): line 0 starts at byte 0.
 * Text hunks use half-open [line_first, line_last); AI-facing helpers in
 * np_texttools convert their 1-based lines to this form. */
size_t np_patch_line_byte_start(const np_patch_buffer_t *b, size_t line) {
    if (!b || !b->data) return 0;
    size_t seen = 0, start = 0;
    for (size_t i = 0; i < b->len; ++i) {
        if (seen == line) return start;
        if (b->data[i] == '\n') { seen++; start = i + 1; }
    }
    return line <= seen ? start : b->len;
}

size_t np_patch_line_byte_end(const np_patch_buffer_t *b, size_t line) {
    if (!b || !b->data) return 0;
    size_t start = np_patch_line_byte_start(b, line);
    size_t end = start;
    while (end < b->len && b->data[end] != '\n') end++;
    return end; /* excludes '\n' */
}

/* --- patch set ------------------------------------------------------- */
np_patch_set_t *np_patch_set_create(uint64_t base_revision) {
    np_patch_set_t *ps = (np_patch_set_t *)calloc(1, sizeof(np_patch_set_t));
    if (!ps) return NULL;
    ps->base_revision = base_revision;
    return ps;
}

void np_patch_set_destroy(np_patch_set_t *ps) {
    if (!ps) return;
    for (size_t i = 0; i < ps->count; ++i) {
        np_patch_hunk_t *h = &ps->hunks[i];
        if (h->track == NP_TRACK_TEXT)   free((void *)h->u.text.text);
        if (h->track == NP_TRACK_BINARY) free((void *)h->u.byte.bytes);
        if (h->track == NP_TRACK_FORMAT) free((void *)h->u.format.payload);
    }
    free(ps->hunks);
    free(ps->undo_data);
    free(ps);
}

uint64_t np_patch_set_revision(const np_patch_set_t *ps) { return ps ? ps->base_revision : 0; }
size_t np_patch_set_count(const np_patch_set_t *ps) { return ps ? ps->count : 0; }
const np_patch_hunk_t *np_patch_set_hunk(const np_patch_set_t *ps, size_t i) {
    return (ps && i < ps->count) ? &ps->hunks[i] : NULL;
}

static np_patch_hunk_t *grow(np_patch_set_t *ps) {
    if (ps->count < ps->cap) return &ps->hunks[ps->count];
    size_t nc = ps->cap ? ps->cap * 2 : 8;
    np_patch_hunk_t *p = (np_patch_hunk_t *)realloc(ps->hunks, nc * sizeof(*p));
    if (!p) return NULL;
    ps->hunks = p; ps->cap = nc;
    return &ps->hunks[ps->count];
}

static void *dup_bytes(const void *src, size_t len) {
    if (!src || len == 0) return NULL;
    void *p = malloc(len);
    if (p) memcpy(p, src, len);
    return p;
}

np_patch_result_t np_patch_add_text(np_patch_set_t *ps, np_patch_op_t op,
                                    size_t line_first, size_t line_last,
                                    const char *text, size_t text_len) {
    if (!ps) return NP_PATCH_ERR_ARG;
    if (op == NP_OP_INSERT && line_first != line_last) return NP_PATCH_ERR_ARG;
    np_patch_hunk_t *h = grow(ps);
    if (!h) return NP_PATCH_ERR_ALLOC;
    memset(h, 0, sizeof(*h));
    h->track = NP_TRACK_TEXT;
    h->op = op;
    h->base_revision = ps->base_revision;
    h->u.text.line_first = line_first;
    h->u.text.line_last  = line_last;
    if (text && text_len) {
        h->u.text.text = (const char *)dup_bytes(text, text_len);
        h->u.text.text_len = text_len;
    }
    ps->count++;
    return NP_PATCH_OK;
}

np_patch_result_t np_patch_add_byte(np_patch_set_t *ps, np_patch_op_t op,
                                    size_t offset, size_t len,
                                    const unsigned char *bytes, size_t bytes_len) {
    if (!ps) return NP_PATCH_ERR_ARG;
    if (op == NP_OP_DELETE && bytes_len) return NP_PATCH_ERR_ARG;
    np_patch_hunk_t *h = grow(ps);
    if (!h) return NP_PATCH_ERR_ALLOC;
    memset(h, 0, sizeof(*h));
    h->track = NP_TRACK_BINARY;
    h->op = op;
    h->base_revision = ps->base_revision;
    h->u.byte.offset = offset;
    h->u.byte.len = len;
    if (bytes && bytes_len) {
        h->u.byte.bytes = (const unsigned char *)dup_bytes(bytes, bytes_len);
        h->u.byte.bytes_len = bytes_len;
    }
    ps->count++;
    return NP_PATCH_OK;
}

np_patch_result_t np_patch_add_format(np_patch_set_t *ps, np_patch_op_t op,
                                      const char *part, const char *path,
                                      const char *payload, size_t payload_len) {
    if (!ps || !part || !path) return NP_PATCH_ERR_ARG;
    np_patch_hunk_t *h = grow(ps);
    if (!h) return NP_PATCH_ERR_ALLOC;
    memset(h, 0, sizeof(*h));
    h->track = NP_TRACK_FORMAT;
    h->op = op;
    h->base_revision = ps->base_revision;
    snprintf(h->u.format.part, sizeof(h->u.format.part), "%s", part);
    snprintf(h->u.format.path, sizeof(h->u.format.path), "%s", path);
    if (payload && payload_len) {
        h->u.format.payload = (const char *)dup_bytes(payload, payload_len);
        h->u.format.payload_len = payload_len;
    }
    ps->count++;
    return NP_PATCH_OK;
}

/* --- validation ------------------------------------------------------ */
np_patch_result_t np_patch_validate(const np_patch_set_t *ps, np_track_t track,
                                    const np_patch_buffer_t *b) {
    if (!ps || !b) return NP_PATCH_ERR_ARG;
    if (ps->base_revision != b->revision) return NP_PATCH_ERR_REVISION;
    for (size_t i = 0; i < ps->count; ++i) {
        const np_patch_hunk_t *h = &ps->hunks[i];
        if (h->track != track) return NP_PATCH_ERR_TRACK;
        if (h->track == NP_TRACK_TEXT) {
            size_t lines = np_patch_line_count(b);
            if (h->u.text.line_first > lines || h->u.text.line_last > lines ||
                h->u.text.line_last < h->u.text.line_first) return NP_PATCH_ERR_RANGE;
        } else if (h->track == NP_TRACK_BINARY) {
            if (h->u.byte.offset > b->len || h->u.byte.len > b->len - h->u.byte.offset)
                return NP_PATCH_ERR_RANGE;
        } else if (h->track == NP_TRACK_FORMAT) {
            /* Protocol is valid; actual write requires F8 deterministic writer. */
            return NP_PATCH_ERR_NEEDS_F8;
        }
    }
    return NP_PATCH_OK;
}

/* --- byte-level splice shared by both tracks ------------------------ */
static np_patch_result_t splice(np_patch_buffer_t *b, size_t at, size_t del_len,
                                const unsigned char *ins, size_t ins_len) {
    if (at > b->len || del_len > b->len - at) return NP_PATCH_ERR_RANGE;
    size_t newlen = b->len - del_len + ins_len;
    if (newlen > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < newlen) nc *= 2;
        unsigned char *p = (unsigned char *)realloc(b->data, nc);
        if (!p) return NP_PATCH_ERR_ALLOC;
        b->data = p; b->cap = nc;
    }
    if (del_len != ins_len) {
        size_t tail = b->len - at - del_len;
        memmove(b->data + at + ins_len, b->data + at + del_len, tail);
    }
    if (ins_len) memcpy(b->data + at, ins, ins_len);
    b->len = newlen;
    return NP_PATCH_OK;
}

/* Resolve a text hunk's line range into a byte range (including the
 * trailing newline for whole-line replacement). */
static void text_byte_range(const np_patch_buffer_t *b, const np_text_hunk_t *th,
                            size_t *out_at, size_t *out_del) {
    size_t at = np_patch_line_byte_start(b, th->line_first);
    size_t end = th->line_last > th->line_first
                   ? np_patch_line_byte_end(b, th->line_last - 1)
                   : at;
    if (end < b->len && b->data[end] == '\n') end++;
    *out_at = at;
    *out_del = end - at;
}

np_patch_result_t np_patch_apply(np_patch_set_t *ps, np_track_t track,
                                 np_patch_buffer_t *b) {
    np_patch_result_t v = np_patch_validate(ps, track, b);
    if (v != NP_PATCH_OK) return v;
    if (ps->count == 0) return NP_PATCH_OK;

    /* capture undo snapshot */
    free(ps->undo_data);
    ps->undo_data = (unsigned char *)dup_bytes(b->data, b->len);
    ps->undo_len = b->len;
    ps->undo_revision = b->revision;
    ps->has_undo = true;

    for (size_t i = 0; i < ps->count; ++i) {
        const np_patch_hunk_t *h = &ps->hunks[i];
        np_patch_result_t r = NP_PATCH_OK;
        if (track == NP_TRACK_BINARY) {
            const np_byte_hunk_t *bh = &h->u.byte;
            r = splice(b, bh->offset, bh->len, bh->bytes, bh->bytes_len);
        } else if (track == NP_TRACK_TEXT) {
            size_t at, del;
            text_byte_range(b, &h->u.text, &at, &del);
            r = splice(b, at, del,
                       (const unsigned char *)h->u.text.text, h->u.text.text_len);
        }
        if (r != NP_PATCH_OK) {
            /* rollback partial application from snapshot */
            np_patch_buffer_set(b, ps->undo_data, ps->undo_len);
            b->revision = ps->undo_revision;
            ps->has_undo = false;
            return r;
        }
    }
    b->revision++;
    return NP_PATCH_OK;
}

bool np_patch_revert(np_patch_set_t *ps, np_patch_buffer_t *b) {
    if (!ps || !b || !ps->has_undo) return false;
    if (!np_patch_buffer_set(b, ps->undo_data, ps->undo_len)) return false;
    b->revision = ps->undo_revision;
    ps->has_undo = false;
    return true;
}
