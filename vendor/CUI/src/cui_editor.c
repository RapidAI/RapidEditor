/* Code editor: editable, highlighted code for tool builders (shader editors,
 * script consoles, config files). It runs on the text fields' engine
 * (cui_textedit.c: caret, undo history, insertion, word motion) and the code
 * block's highlighter (cui_code.c). Lines sit on a monospace grid without
 * wrapping, and only the lines in view are tokenized and drawn, so files of
 * many thousands of lines stay fast. */
#include "cui_internal.h"


#define CUI__ED_SALT  0xED17C0DEu
#define CUI__ED_BAR   12.0f     /* scrollbar zones along the right and bottom edges */
#define CUI__ED_INSET 6.0f      /* above the first line and below the last */
#define CUI__ED_GAP   8.0f      /* between the gutter and the code */
#define CUI__ED_UNDO_MAX 64

/* An undo step stores only the bytes that changed. The generic text-field
 * history keeps 48 whole-buffer snapshots, which is unusable on large files
 * (48 x 100 MB and a 100 MB memcpy on every keystroke). */
typedef struct {
    int pos;                   /* byte offset of the edit */
    int old_n, new_n;          /* bytes removed / inserted */
    char *old, *neu;           /* owned copies of those bytes */
    int cursor;                /* caret before the edit */
    double t;
    int kind;
} cui__ed_hist;

typedef struct {
    cui_edit_state ed;          /* first: caret, anchor and undo history, as in every text field */
    double sy;                  /* scroll, points */
    float sx;
    int pref_col;               /* column kept by up and down, -1 none */
    int drag, drag_a, drag_b;   /* pointer drag unit (1 char, 2 word, 3 line) and the unit pressed */
    int bar_drag;               /* scrollbar thumb held: 1 vertical, 2 horizontal */
    float bar_grab, bar_t;
    double last_activity;
    int mode;                   /* bar: 0 none, 1 find, 2 go to line */
    char query[128], go[24];
    bool find_case;             /* find bar honours letter case */
    bool find_word;             /* find bar matches whole words only */
    bool tab_free;              /* Esc released Tab: the next one moves focus */
    bool bar_focus;             /* give the bar's field focus this frame */
    bool pend_sel, pend_goto, pend_find, init;
    int pend_a, pend_c, pend_line, pend_col;
    float focus_t;

    /* ---- large-buffer state (all owned, released by cui__ed_dtor) ---- */
    int *li;                   /* cached line starts */
    int li_cap, li_nl, li_len, li_maxlen;
    uint64_t li_rev;
    bool li_valid;
    int bs_line;               /* block-comment prefix cache */
    bool bs_in, bs_have;
    uint64_t bs_rev;
    const char *const *bs_kw;
    cui__ed_hist hist[CUI__ED_UNDO_MAX];
    int hist_count, hist_pos;
    char *ghost;               /* inline suggestion shown after the caret (owned) */
    int ghost_len;             /* 0 = no suggestion */
    int ghost_cursor;          /* caret the suggestion belongs to; any move clears */

    /* scrollbar hit markers for the find query: cached line numbers, rebuilt
     * when the query, its toggles or the text changes */
    int *tk_line;              /* owned, released by the dtor */
    int tk_cap, tk_n;
    uint64_t tk_rev;
    int tk_len;
    bool tk_case, tk_word, tk_valid;
    char tk_query[128];
} cui__ed_state;

static void cui__ed_dtor(cui_ctx *ui, void *data) {
    cui__ed_state *s = (cui__ed_state *)data;
    for (int i = 0; i < s->hist_count; i++) {
        cui__free(ui, s->hist[i].old);
        cui__free(ui, s->hist[i].neu);
    }
    for (int i = 0; i < CUI__UNDO_MAX; i++) cui__free(ui, s->ed.undo[i]);
    cui__free(ui, s->li);
    cui__free(ui, s->ghost);
    cui__free(ui, s->tk_line);
}

/* One frame of editing: the caller's buffer and the line index. */
typedef struct {
    cui_ctx *ui;
    cui__ed_state *st;
    char *buf;
    size_t cap;
    int len;
    uint64_t rev;               /* caller's buffer revision, for the index cache */
    int *ls, nl, maxlen;        /* line starts (ls[nl] = len + 1), count, longest line in bytes */
    int tab;
    bool use_tabs, ro;
    bool changed, refused, reveal, center;
} cui__ed;

/* ---- text model ---- */

/* The line index used to be rebuilt and frame-allocated every frame, which
 * means three full scans of the buffer plus an allocation of 4 bytes per
 * line on each frame. For large files we keep it in the editor state and
 * rebuild only when the text changed (revision bumped by the app, or an
 * edit made inside the editor, which passes force=true). */
static void cui__ed_index(cui__ed *E, bool force) {
    cui__ed_state *st = E->st;
    if (!force && st->li_valid && st->li && st->li_len == E->len && st->li_rev == E->rev) {
        E->ls = st->li;
        E->nl = st->li_nl;
        E->maxlen = st->li_maxlen;
        return;
    }
    const char *b = E->buf, *end = b + E->len, *p = b;
    int n = 1;
    while ((p = memchr(p, '\n', (size_t)(end - p))) != NULL) { n++; p++; }
    if (!st->li || n + 1 > st->li_cap) {
        cui__free(E->ui, st->li);
        st->li_cap = n + 1 + n / 4;             /* room to grow without a realloc */
        st->li = (int *)cui__alloc(E->ui, sizeof(int) * (size_t)st->li_cap);
    }
    int k = 1, maxlen = 0;
    st->li[0] = 0;
    for (p = b; (p = memchr(p, '\n', (size_t)(end - p))) != NULL; p++) {
        int at = (int)(p - b);
        if (at - st->li[k - 1] > maxlen) maxlen = at - st->li[k - 1];
        st->li[k++] = at + 1;
    }
    if (E->len - st->li[n - 1] > maxlen) maxlen = E->len - st->li[n - 1];
    st->li[n] = E->len + 1;
    st->li_nl = n;
    st->li_len = E->len;
    st->li_maxlen = maxlen;
    st->li_rev = E->rev;
    st->li_valid = true;
    st->bs_have = false;                        /* the prefix cache is stale */
    E->ls = st->li;
    E->nl = n;
    E->maxlen = maxlen;
}

static int cui__ed_end(const cui__ed *E, int line) { return E->ls[line + 1] - 1; }

static int cui__ed_line_of(const cui__ed *E, int pos) {
    int lo = 0, hi = E->nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (E->ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Column after s[a, b) when it starts at column c: tabs go to the next stop,
 * every code point is one column. */
static int cui__ed_adv(const char *s, int a, int b, int tab, int c) {
    for (int i = a; i < b; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\t') c = (c / tab + 1) * tab;
        else if ((ch & 0xC0) != 0x80) c++;
    }
    return c;
}
/* Column of byte b in the line that starts at a. */
static int cui__ed_col(const char *s, int a, int b, int tab) { return cui__ed_adv(s, a, b, tab, 0); }

/* The byte boundary in [a, e) nearest to column x. */
static int cui__ed_at_col(const char *s, int a, int e, float x, int tab) {
    int c = 0, i = a;
    while (i < e) {
        int nc = s[i] == '\t' ? (c / tab + 1) * tab : c + 1;
        if (x < (float)(c + nc) * 0.5f) break;
        c = nc;
        i = cui__utf8_next(s, e, i);
    }
    return i;
}

static int cui__ed_pos(const cui__ed *E, int line, float col) {
    line = CUI_CLAMP(line, 0, E->nl - 1);
    return cui__ed_at_col(E->buf, E->ls[line], cui__ed_end(E, line), col, E->tab);
}

static int cui__ed_caret_col(const cui__ed *E, int pos) {
    int line = cui__ed_line_of(E, pos);
    return cui__ed_col(E->buf, E->ls[line], pos, E->tab);
}

/* ---- pixel positions ----
 * A monospace grid only fits single-width glyphs. CJK and emoji glyphs that
 * come from a fallback font are wider (about 1.67 ASCII cells here), so the
 * caret, selection, marks, text runs and hit testing must use the real font
 * advances. The integer "column" helpers above stay for line/column display,
 * go-to-line and indentation, where one code point is one column. */

/* Width in points of s[a, b), with tabs expanded to the next stop. */
static float cui__ed_span(cui_ctx *ui, float size, float cw, int tab,
                          const char *s, int a, int b) {
    float x = 0;
    int i = a;
    while (i < b) {
        int j = i;
        while (j < b && s[j] != '\t') {
            uint32_t cp;
            int n = cui__utf8_decode(s + j, b - j, &cp);
            j += n > 0 ? n : 1;
        }
        if (j > i) x += cui__text_width(ui, CUI_FONT_MONO, size, s + i, j - i, true);
        if (j < b && s[j] == '\t') {
            float stop = (float)tab * cw;
            x = (floorf(x / stop) + 1.0f) * stop;
            j++;
        }
        i = j;
    }
    return x;
}

/* Byte boundary in [a, e) nearest to the pixel offset px from the line start. */
static int cui__ed_at_px(cui_ctx *ui, float size, float cw, int tab,
                         const char *s, int a, int e, float px) {
    float acc = 0;
    int i = a;
    while (i < e) {
        if (s[i] == '\t') {
            float stop = (float)tab * cw;
            float next = (floorf(acc / stop) + 1.0f) * stop;
            if (px < (acc + next) * 0.5f) break;
            acc = next;
            i++;
            continue;
        }
        uint32_t cp;
        int n = cui__utf8_decode(s + i, e - i, &cp);
        if (n <= 0) n = 1;
        float w = cui__text_width(ui, CUI_FONT_MONO, size, s + i, n, true);
        if (px < acc + w * 0.5f) break;
        acc += w;
        i += n;
    }
    return i;
}

static int cui__ed_pos_px(cui_ctx *ui, float size, float cw, const cui__ed *E,
                          int line, float px) {
    line = CUI_CLAMP(line, 0, E->nl - 1);
    return cui__ed_at_px(ui, size, cw, E->tab, E->buf, E->ls[line],
                         cui__ed_end(E, line), px);
}

/* first character of the line that is not a space or a tab */
static int cui__ed_first_text(const cui__ed *E, int line) {
    int i = E->ls[line], e = cui__ed_end(E, line);
    while (i < e && (E->buf[i] == ' ' || E->buf[i] == '\t')) i++;
    return i;
}

/* Replace [a, b) with t. An edit that does not fit in the buffer is refused
 * whole and reported; the caret lands after the new text. */
/* ---- delta undo ----------------------------------------------------- */

static void cui__ed_hist_free(cui_ctx *ui, cui__ed_hist *h) {
    cui__free(ui, h->old);
    cui__free(ui, h->neu);
    memset(h, 0, sizeof *h);
}

static void cui__ed_hist_push(cui__ed *E, int a, int b, const char *t, int n, int kind) {
    cui__ed_state *st = E->st;
    cui_ctx *ui = E->ui;
    /* Merge a run of plain insertions at the caret, like the text fields do. */
    if (kind == 1 && b == a && st->hist_pos == st->hist_count && st->hist_count > 0) {
        cui__ed_hist *h = &st->hist[st->hist_count - 1];
        if (h->kind == 1 && h->old_n == 0 && a == h->pos + h->new_n &&
            ui->time - h->t < 1.0) {
            char *p = (char *)cui__realloc(ui, h->neu, (size_t)h->new_n + (size_t)n + 1);
            if (p) {
                memcpy(p + h->new_n, t, (size_t)n);
                h->new_n += n;
                p[h->new_n] = 0;
                h->neu = p;
                h->t = ui->time;
                return;
            }
        }
    }
    for (int i = st->hist_pos; i < st->hist_count; i++) cui__ed_hist_free(ui, &st->hist[i]);
    st->hist_count = st->hist_pos;
    if (st->hist_count == CUI__ED_UNDO_MAX) {
        cui__ed_hist_free(ui, &st->hist[0]);
        memmove(st->hist, st->hist + 1, sizeof(cui__ed_hist) * (CUI__ED_UNDO_MAX - 1));
        memset(&st->hist[CUI__ED_UNDO_MAX - 1], 0, sizeof(cui__ed_hist));
        st->hist_count--;
    }
    cui__ed_hist *h = &st->hist[st->hist_count++];
    memset(h, 0, sizeof *h);
    h->pos = a;
    h->old_n = b - a;
    h->new_n = n;
    h->cursor = st->ed.cursor;
    h->t = ui->time;
    h->kind = kind;
    if (h->old_n > 0) {
        h->old = (char *)cui__alloc(ui, (size_t)h->old_n + 1);
        memcpy(h->old, E->buf + a, (size_t)h->old_n);
        h->old[h->old_n] = 0;
    }
    if (h->new_n > 0) {
        h->neu = (char *)cui__alloc(ui, (size_t)h->new_n + 1);
        memcpy(h->neu, t, (size_t)h->new_n);
        h->neu[h->new_n] = 0;
    }
    st->hist_pos = st->hist_count;
}

static bool cui__ed_apply(cui__ed *E, int pos, int del, const char *ins, int ins_n) {
    int new_len = E->len - del + ins_n;
    if (new_len < 0 || (size_t)new_len + 1 > E->cap) return false;
    memmove(E->buf + pos + ins_n, E->buf + pos + del,
            (size_t)(E->len - pos - del) + 1);
    if (ins_n) memcpy(E->buf + pos, ins, (size_t)ins_n);
    E->len = new_len;
    E->changed = E->reveal = true;
    E->st->pref_col = -1;
    cui__ed_index(E, true);
    return true;
}

static bool cui__ed_hist_step(cui__ed *E, bool redo) {
    cui__ed_state *st = E->st;
    if (!redo) {
        if (st->hist_pos <= 0) return false;
        cui__ed_hist *h = &st->hist[st->hist_pos - 1];
        if (!cui__ed_apply(E, h->pos, h->new_n, h->old, h->old_n)) return false;
        st->hist_pos--;
        st->ed.cursor = st->ed.anchor = CUI_MIN(h->cursor, E->len);
    } else {
        if (st->hist_pos >= st->hist_count) return false;
        cui__ed_hist *h = &st->hist[st->hist_pos];
        if (!cui__ed_apply(E, h->pos, h->old_n, h->neu, h->new_n)) return false;
        st->hist_pos++;
        st->ed.cursor = st->ed.anchor = CUI_MIN(h->pos + h->new_n, E->len);
    }
    return true;
}

/* ---- editing -------------------------------------------------------- */

static void cui__ed_ghost_clear(cui__ed_state *st) {
    st->ghost_len = 0;          /* the buffer stays allocated for reuse */
}

static bool cui__ed_replace(cui__ed *E, int a, int b, const char *t, int n, int kind) {
    if (E->ro) return false;
    int m = 0;
    for (int i = 0; i < n; i++) if (t[i] != '\r') m++;
    if ((size_t)(E->len - (b - a) + m) + 1 > E->cap) { E->refused = true; return false; }
    if (m == 0 && b == a) return false;
    if (m != n) {
        char *clean = (char *)cui__frame_alloc(E->ui, (size_t)m + 1);
        int k = 0;
        for (int i = 0; i < n; i++) if (t[i] != '\r') clean[k++] = t[i];
        clean[k] = 0;
        t = clean;
    }
    cui_edit_state *e = &E->st->ed;
    cui__ed_ghost_clear(E->st);         /* any edit invalidates the suggestion */
    cui__ed_hist_push(E, a, b, t, m, kind);
    cui__edit_delete(E->buf, &E->len, a, b);
    int got = cui__edit_insert(E->buf, &E->len, E->cap, a, t, m);
    e->cursor = e->anchor = e->last_cursor = a + got;
    E->changed = E->reveal = true;
    E->st->pref_col = -1;
    cui__ed_index(E, true);
    return true;
}

static void cui__ed_move(cui__ed *E, int to, bool extend) {
    cui_edit_state *e = &E->st->ed;
    e->cursor = CUI_CLAMP(to, 0, E->len);
    if (!extend) e->anchor = e->cursor;
    E->reveal = true;
}

static int cui__ed_class(unsigned char c) { return cui__edit_is_word(c) ? 1 : (c == ' ' || c == '\t') ? 2 : 3; }

/* The word, space run or symbol at pos (double-click). */
static void cui__ed_word_at(const cui__ed *E, int pos, int *a, int *b) {
    const char *s = E->buf;
    int at = pos;
    if (at >= E->len || s[at] == '\n') at = pos - 1;
    if (at < 0 || s[at] == '\n') { *a = *b = pos; return; }
    int k = cui__ed_class((unsigned char)s[at]);
    if (k == 3) { *a = at; *b = cui__utf8_next(s, E->len, at); return; }
    int i = at, j = at;
    while (i > 0 && s[i - 1] != '\n' && cui__ed_class((unsigned char)s[i - 1]) == k) i--;
    while (j < E->len && s[j] != '\n' && cui__ed_class((unsigned char)s[j]) == k) j++;
    *a = i; *b = j;
}

/* The whole line at pos, with its newline (triple-click, the gutter). */
static void cui__ed_line_at(const cui__ed *E, int pos, int *a, int *b) {
    int line = cui__ed_line_of(E, pos);
    *a = E->ls[line];
    *b = CUI_MIN(E->ls[line + 1], E->len);
}

/* ---- brackets ---- */

static int cui__ed_partner(char c, int *dir) {
    switch (c) {
    case '(': *dir = 1; return ')';
    case '[': *dir = 1; return ']';
    case '{': *dir = 1; return '}';
    case ')': *dir = -1; return '(';
    case ']': *dir = -1; return '[';
    case '}': *dir = -1; return '{';
    default: return 0;
    }
}

static int cui__ed_match(const char *s, int len, int at) {
    int dir = 0, other = cui__ed_partner(s[at], &dir), depth = 0;
    for (int i = at; i >= 0 && i < len; i += dir) {
        if (s[i] == s[at]) depth++;
        else if (s[i] == other && --depth == 0) return i;
    }
    return -1;
}

/* The bracket beside the caret (before it first) and its partner. */
static void cui__ed_brackets(const cui__ed *E, int caret, int *a, int *b) {
    int dir;
    *a = *b = -1;
    for (int k = 0; k < 2; k++) {
        int at = k == 0 ? caret - 1 : caret;
        if (at < 0 || at >= E->len || !cui__ed_partner(E->buf[at], &dir)) continue;
        int m = cui__ed_match(E->buf, E->len, at);
        if (m >= 0) { *a = at; *b = m; return; }
    }
}

/* ---- find ---- */


/* Word character for whole-word matching: ASCII letters, digits, '_'.
 * Multi-byte UTF-8 bytes never count as word chars, so a whole-word
 * query that is not a word itself simply never matches at its edges. */
static bool cui__ed_word_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* mc: case sensitive match (default false keeps the historical
 * case-insensitive behaviour of the find bar). ww: the match must be a
 * whole word - no word character may touch either edge. */
static bool cui__ed_hit(const char *s, int len, int i, const char *q, int qn,
                        bool mc, bool ww) {
    if (qn <= 0 || i < 0 || i + qn > len) return false;
    if (mc) { if (memcmp(s + i, q, (size_t)qn) != 0) return false; }
    else {
        for (int k = 0; k < qn; k++) if (cui__ascii_lower((unsigned char)s[i + k]) != cui__ascii_lower((unsigned char)q[k])) return false;
    }
    if (ww) {
        if (i > 0 && cui__ed_word_byte((unsigned char)s[i - 1])) return false;
        if (i + qn < len && cui__ed_word_byte((unsigned char)s[i + qn])) return false;
    }
    return true;
}

/* First match at or after from (dir > 0), or last one before from (dir < 0),
 * wrapping around the ends; -1 when there is none. */
static int cui__ed_find(const char *s, int len, const char *q, int qn, int from, int dir, bool mc, bool ww) {
    if (qn <= 0 || qn > len) return -1;
    int last = len - qn;
    if (dir > 0) {
        for (int i = CUI_MAX(0, from); i <= last; i++) if (cui__ed_hit(s, len, i, q, qn, mc, ww)) return i;
        for (int i = 0; i < CUI_MIN(from, last + 1); i++) if (cui__ed_hit(s, len, i, q, qn, mc, ww)) return i;
    } else {
        for (int i = CUI_MIN(from - 1, last); i >= 0; i--) if (cui__ed_hit(s, len, i, q, qn, mc, ww)) return i;
        for (int i = last; i >= CUI_MAX(0, from); i--) if (cui__ed_hit(s, len, i, q, qn, mc, ww)) return i;
    }
    return -1;
}

static void cui__ed_find_go(cui__ed *E, int from, int dir) {
    const char *q = E->st->query;
    int qn = (int)strlen(q);
    int at = cui__ed_find(E->buf, E->len, q, qn, from, dir, E->st->find_case, E->st->find_word);
    if (at < 0) return;
    E->st->ed.anchor = at;
    E->st->ed.cursor = at + qn;
    E->reveal = E->center = true;
    E->st->pref_col = -1;
}

/* ---- indentation ---- */

/* Tab and Shift+Tab over the selected lines (or the caret's line). */
static void cui__ed_indent(cui__ed *E, bool out) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    int l0 = cui__ed_line_of(E, s0), l1 = cui__ed_line_of(E, s1);
    if (s1 > s0 && l1 > l0 && s1 == E->ls[l1]) l1--;   /* a selection that ends at a line start leaves that line alone */
    int lines = l1 - l0 + 1, a = E->ls[l0], b = cui__ed_end(E, l1);
    int *start = cui__frame_alloc(E->ui, sizeof(int) * (size_t)lines * 2), *delta = start + lines;
    char *t = cui__frame_alloc(E->ui, (size_t)(b - a) + (size_t)lines * (size_t)E->tab + 1);
    int n = 0, total = 0;
    for (int k = 0; k < lines; k++) {
        int ls = E->ls[l0 + k], le = cui__ed_end(E, l0 + k), i = ls;
        start[k] = ls;
        delta[k] = 0;
        if (!out && le > ls) {
            if (E->use_tabs) t[n++] = '\t';
            else for (int j = 0; j < E->tab; j++) t[n++] = ' ';
            delta[k] = E->use_tabs ? 1 : E->tab;
        } else if (out) {
            if (i < le && E->buf[i] == '\t') i++;
            else while (i < le && i - ls < E->tab && E->buf[i] == ' ') i++;
            delta[k] = -(i - ls);
        }
        memcpy(t + n, E->buf + i, (size_t)(le - i));
        n += le - i;
        if (k < lines - 1) t[n++] = '\n';
        total += delta[k];
    }
    bool any = false;
    for (int k = 0; k < lines; k++) any |= delta[k] != 0;
    if (!any) return;
    int c0 = e->cursor, a0 = e->anchor;
    if (!cui__ed_replace(E, a, b, t, n, 3)) return;
    /* keep the selection on the same text */
    int map[2], src[2] = { c0, a0 };
    for (int m = 0; m < 2; m++) {
        int p = src[m];
        if (p < a) { map[m] = p; continue; }
        if (p > b) { map[m] = p + total; continue; }
        int k = lines - 1, before = 0;
        while (k > 0 && start[k] > p) k--;
        for (int j = 0; j < k; j++) before += delta[j];
        int own = out ? -CUI_MIN(-delta[k], p - start[k]) : (p > start[k] ? delta[k] : 0);
        map[m] = p + before + own;
    }
    e->cursor = e->last_cursor = map[0];
    e->anchor = map[1];
}

/* Enter: a new line at the indentation of this one, a level deeper after an
 * opening brace (C-like languages) or a colon (Python); between a pair of
 * braces the closing one moves down a line of its own. */
static void cui__ed_newline(cui__ed *E, int lang_kind) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    int line = cui__ed_line_of(E, s0), ls = E->ls[line];
    int ind_end = CUI_MIN(cui__ed_first_text(E, line), s0);
    int k = s0;
    while (k > ls && (E->buf[k - 1] == ' ' || E->buf[k - 1] == '\t')) k--;
    char before = k > ls ? E->buf[k - 1] : 0;
    bool deeper = (lang_kind == 1 && (before == '{' || before == '[' || before == '(')) || (lang_kind == 2 && before == ':');
    int j = s1;
    while (j < E->len && (E->buf[j] == ' ' || E->buf[j] == '\t')) j++;
    char after = j < E->len ? E->buf[j] : 0;
    bool split = lang_kind == 1 && ((before == '{' && after == '}') || (before == '[' && after == ']') || (before == '(' && after == ')'));
    int ind = ind_end - ls, unit = E->use_tabs ? 1 : E->tab;
    char *t = cui__frame_alloc(E->ui, (size_t)(ind * 2 + unit + 3));
    int n = 0;
    t[n++] = '\n';
    memcpy(t + n, E->buf + ls, (size_t)ind); n += ind;
    if (deeper) { if (E->use_tabs) t[n++] = '\t'; else for (int i = 0; i < unit; i++) t[n++] = ' '; }
    int caret = n;
    if (split) { t[n++] = '\n'; memcpy(t + n, E->buf + ls, (size_t)ind); n += ind; }
    if (!cui__ed_replace(E, s0, split ? j : s1, t, n, 3)) return;
    e->cursor = e->anchor = e->last_cursor = s0 + caret;
}

/* Typing: replaces the selection. A closing brace typed on a line of only
 * indentation steps back a level. */
static void cui__ed_type(cui__ed *E, const char *text, int n, int lang_kind) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (lang_kind == 1 && n == 1 && (text[0] == '}' || text[0] == ']' || text[0] == ')') && s0 == s1) {
        int line = cui__ed_line_of(E, s0), ls = E->ls[line];
        if (cui__ed_first_text(E, line) == s0 && s0 > ls && text[0] == '}') {
            int i = s0;
            if (E->buf[i - 1] == '\t') i--;
            else while (i > ls && s0 - i < E->tab && E->buf[i - 1] == ' ') i--;
            char *t = cui__frame_alloc(E->ui, (size_t)(i - ls) + 2);
            memcpy(t, E->buf + ls, (size_t)(i - ls));
            t[i - ls] = '}';
            cui__ed_replace(E, ls, s0, t, i - ls + 1, 1);
            return;
        }
    }
    cui__ed_replace(E, s0, s1, text, n, s1 > s0 ? 3 : 1);
}

/* Whether a block comment is open after s[i, e) when one was open before
 * (in): a quick scan that follows the tokenizer (strings end at the line
 * end; a line without a '/' cannot open one). */
static bool cui__ed_scan_line(const cui__lang *L, const char *s, int i, int e, bool in) {
    if (!in && !memchr(s + i, '/', (size_t)(e - i))) return false;
    size_t lc = L->line_comment ? strlen(L->line_comment) : 0;
    while (i < e) {
        if (in) {
            while (i < e && !(s[i] == '*' && i + 1 < e && s[i + 1] == '/')) i++;
            if (i < e) { i += 2; in = false; }
            continue;
        }
        char c = s[i];
        if (lc && (size_t)(e - i) >= lc && strncmp(s + i, L->line_comment, lc) == 0) break;
        if (c == '/' && i + 1 < e && s[i + 1] == '*') { in = true; continue; }
        if (c == '"' || c == '\'' || (c == '`' && L->backtick)) {
            i++;
            while (i < e && s[i] != c) { if (s[i] == '\\' && i + 1 < e) i++; i++; }
            if (i < e) i++;
            continue;
        }
        i++;
    }
    return in;
}

/* Block comment open at the start of line `upto`. */
static bool cui__ed_block_state(const cui__ed *E, const cui__lang *L, int upto) {
    if (!L->block_comments) return false;
    /* Scrolling walks the prefix one screen at a time; remember where the
     * last scan ended instead of walking from line 0 on every frame. */
    cui__ed_state *st = E->st;
    int start = 0;
    bool in = false;
    if (st->bs_have && st->bs_rev == E->rev && st->bs_kw == L->kw && upto >= st->bs_line) {
        start = st->bs_line;
        in = st->bs_in;
    }
    for (int line = start; line < upto; line++)
        in = cui__ed_scan_line(L, E->buf, E->ls[line], cui__ed_end(E, line), in);
    st->bs_have = true;
    st->bs_rev = E->rev;
    st->bs_kw = L->kw;
    st->bs_line = upto;
    st->bs_in = in;
    return in;
}

/* ---- painting ---- */

typedef struct {
    cui_id sid;
    const char *text;           /* a copy of the lines in view; text[0] is byte `base` */
    int base, first, count, nl;
    const int *ls;              /* starts of the lines in view and the one after */
    bool in_block, json, empty, focused, show_line, ro, guides;
    bool lig;                   /* opt-in programming ligatures (needs CUI_TEXT_SHAPING) */
    int ws;                     /* whitespace glyphs: 0 off, 1 leading, 2 all */
    cui__lang lang;
    int tab, cursor, anchor, caret_line, caret_col;
    float caret_px;             /* caret in real text pixels, not grid columns */
    int br_line[2];
    float br_px[2];
    const char *query; int qn;
    bool qcase;                 /* find matches honour letter case */
    bool qword;                 /* find matches whole words only */
    const cui_text_mark *marks; int nmarks;   /* the app's marks that reach the lines in view */
    const int *tick_lines; int nticks;        /* find hits on the scrollbar, cached line numbers */
    cui_color mark_colors[7];
    const char *placeholder;
    float size, lh, cw, gutter, view_h;
    double sy, max_sy;
    float sx, max_sx, bar_t;
    int bar_big;                /* 1 vertical thumb held or hovered, 2 horizontal */
    double caret_phase;
    cui_color colors[CUI__TK_COUNT];
    cui_color num, num_on, band, sel, match, match_edge, brk, brk_edge, caret, muted, line, bar, tick;
    cui_color guide, guide_on;  /* indent guides; guide_on marks the caret's line */
    cui_color ws_glyph, ws_end; /* whitespace glyphs; ws_end marks trailing */
    int vc0, vc1;               /* columns in view */
    const char *ghost;          /* inline suggestion drawn after the caret, or NULL */
    int ghost_len;
    int ghost_extra;            /* suggestion lines beyond the first (marker shown) */
} cui__ed_paint;

/* Draws s[a, b) starting xoff points from the line start. Tabs expand and
 * only the pixels in view are drawn; advances come from the font so wide
 * glyphs stay aligned with the caret. */
static void cui__ed_run(cui_ctx *ui, const cui__ed_paint *p, const char *s,
                        int a, int b, float xoff, float x0, float y, cui_color c) {
    float minx = (float)p->vc0 * p->cw;
    float maxx = (float)(p->vc1 + 1) * p->cw;
    int i = a;
    float x = xoff;
    while (i < b && x < maxx) {
        if (s[i] == '\t') {
            float stop = (float)p->tab * p->cw;
            x = (floorf(x / stop) + 1.0f) * stop;
            i++;
            continue;
        }
        int j = i;
        while (j < b && s[j] != '\t') {
            uint32_t cp;
            int n = cui__utf8_decode(s + j, b - j, &cp);
            j += n > 0 ? n : 1;
        }
        int i2 = i, j2;
        float x2 = x, x3;
        while (i2 < j && x2 < minx) {
            uint32_t cp;
            int n = cui__utf8_decode(s + i2, j - i2, &cp);
            if (n <= 0) n = 1;
            x2 += cui__text_width(ui, CUI_FONT_MONO, p->size, s + i2, n, true);
            i2 += n;
        }
        j2 = i2;
        x3 = x2;
        while (j2 < j && x3 < maxx) {
            uint32_t cp;
            int n = cui__utf8_decode(s + j2, j - j2, &cp);
            if (n <= 0) n = 1;
            x3 += cui__text_width(ui, CUI_FONT_MONO, p->size, s + j2, n, true);
            j2 += n;
        }
        if (j2 > i2)
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x2, y, c, s + i2, j2 - i2, true);
        x += cui__text_width(ui, CUI_FONT_MONO, p->size, s + i, j - i, true);
        i = j;
    }
}

/* Whitespace glyphs: a mid dot per space, an arrow per tab. In leading mode
 * the walk stops at the line's first text byte; otherwise every space and
 * tab draws. The bytes after the last text byte count as trailing and draw
 * in the warm end color (an all-whitespace line is all trailing). The x
 * advances mirror the text runs, so glyphs sit exactly in their cells. */
static void cui__ed_paint_ws(cui_ctx *ui, const cui__ed_paint *p, const char *s,
                             int ls, int le, bool lead, float x0, float y) {
    static const char dot[] = "\xc2\xb7", arrow[] = "\xe2\x86\x92";
    float minx = (float)p->vc0 * p->cw;
    float maxx = (float)(p->vc1 + 1) * p->cw;
    int end = le;   /* first byte of the trailing whitespace, or le if none */
    while (end > ls && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) end--;
    float x = 0, stop = (float)p->tab * p->cw;
    float dw = cui__text_width(ui, CUI_FONT_MONO, p->size, dot, 2, true);
    for (int i = ls; i < le && !(lead && i >= end); ) {
        if (s[i] == '\t') {
            if (x >= minx - p->cw && x <= maxx)
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x + 0.5f, y,
                                    i >= end ? p->ws_end : p->ws_glyph, arrow, 3, true);
            x = (floorf(x / stop) + 1.0f) * stop;
            i++;
        } else if (s[i] == ' ') {
            if (x >= minx - p->cw && x <= maxx)
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x + (p->cw - dw) * 0.5f, y,
                                    i >= end ? p->ws_end : p->ws_glyph, dot, 2, true);
            x += p->cw;
            i++;
        } else if (s[i] == '\r') {
            i++;   /* a carriage return stays invisible */
        } else {
            int j = i;
            while (j < le && s[j] != '\t' && s[j] != ' ' && s[j] != '\r') {
                uint32_t cp;
                int n = cui__utf8_decode(s + j, le - j, &cp);
                j += n > 0 ? n : 1;
            }
            x += cui__text_width(ui, CUI_FONT_MONO, p->size, s + i, j - i, true);
            i = j;
        }
    }
}

static void cui__ed_thumb(float track, double view, double max, double pos, float *at, float *len) {
    double total = view + max;
    *len = CUI_MAX(28.0f, track * (float)(view / total));
    *at = max > 0 ? (float)(pos / max) * (track - *len) : 0;
}

static void cui__paint_editor(cui_ctx *ui, cui_node *n, void *data) {
    cui__ed_paint *p = data;
    cui_rect r = n->rect;
    {   /* the wheel comes here while the pointer is over the editor and it can scroll */
        cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
        float s = ui->scale;
        cui_rect cp = { clip.x / s, clip.y / s, clip.w / s, clip.h / s };
        cui_scroll_state *ss = cui_state(ui, p->sid, sizeof *ss);
        ss->view_w = r.w; ss->view_h = r.h;
        ss->content_w = r.w + p->max_sx;
        ss->content_h = p->max_sy > 0 ? r.h + 1e6f : r.h;
        cui__paint_add_hit(ui, p->sid, cui__rect_intersect(r, cp), CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
    }
    if (r.h > p->view_h + 0.5f) cui_request_redraw(ui);   /* laid out taller than last frame: more lines next frame */
    bool unshaped = ui->text_unshaped;
    /* The editor usually keeps logical glyph positions (caret x = sum of the
     * per-character advances). With the ligature opt-in the paint scope may
     * run the shaper: bundled mono faces draw ligatures at the sum width of
     * their characters, so positions stay the same and the caret still lands
     * on the character grid. */
#ifdef CUI_TEXT_SHAPING
    ui->text_ligatures = p->lig;
    ui->text_unshaped = !p->lig;
#endif
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    float ty = (p->lh - fh) * 0.5f;
    float gx = r.x + p->gutter;                     /* left edge of the code */
    float x0 = gx + CUI__ED_GAP - p->sx;            /* column 0 */
    cui_rect code = { gx, r.y, CUI_MAX(0, r.w - p->gutter), r.h };
    int s0 = CUI_MIN(p->cursor, p->anchor), s1 = CUI_MAX(p->cursor, p->anchor);
    cui_push_clip(ui, r);
    /* the caret's line, across the gutter and the code */
    if (p->show_line && s0 == s1 && p->caret_line >= p->first && p->caret_line < p->first + p->count) {
        float y = r.y + CUI__ED_INSET + (float)((double)p->caret_line * p->lh - p->sy);
        cui_draw_rect(ui, (cui_rect){ r.x, y, r.w, p->lh }, CUI_CORNERS(0), p->band);
    }
    /* line numbers */
    if (p->gutter > CUI__ED_GAP) {
        for (int k = 0; k < p->count; k++) {
            int line = p->first + k;
            float y = r.y + CUI__ED_INSET + (float)((double)line * p->lh - p->sy);
            const char *num = cui__frame_printf(ui, "%d", line + 1);
            float w = cui__text_width(ui, CUI_FONT_MONO, p->size, num, -1, true);
            bool on = p->show_line && line == p->caret_line;
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, gx - 10 - w, y + ty, on ? p->num_on : p->num, num, -1, true);
        }
        if (p->sx > 0.5f) cui_draw_rect(ui, (cui_rect){ gx, r.y, 1, r.h }, CUI_CORNERS(0), p->line);   /* code scrolled under the gutter */
    }
    cui_push_clip(ui, code);
    cui__tok *toks = NULL;
    int tok_cap = 0;
    bool block = p->in_block;
    for (int k = 0; k < p->count; k++) {
        int line = p->first + k;
        int ls = p->ls[k] - p->base, le = p->ls[k + 1] - 1 - p->base;   /* in text */
        const char *s = p->text;
        float y = r.y + CUI__ED_INSET + (float)((double)line * p->lh - p->sy);
        int als = p->ls[k], ale = p->ls[k + 1] - 1;                    /* in the buffer */
        /* the app's marks, then the find matches, then the selection — the
         * selection is translucent and covers both (VS Code layering) */
        for (int m = 0; m < p->nmarks; m++) {
            const cui_text_mark *mk = &p->marks[m];
            if (mk->start > ale) break;
            if (mk->end <= als && !(mk->end == mk->start && mk->start == als)) continue;
            int a = CUI_MAX(mk->start, als) - p->base, b = CUI_MIN(mk->end, ale) - p->base;
            float xa = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, a);
            float xb = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, b);
            if (xb - xa < 2) { xa -= 1; xb = xa + 2; }   /* an empty match shows as a bar */
            cui_draw_rect(ui, (cui_rect){ x0 + xa, y + 1, xb - xa, p->lh - 2 }, CUI_CORNERS(2), p->mark_colors[CUI_CLAMP(mk->color, 0, 6)]);
        }
        if (p->qn > 0) {
            for (int i = ls; i + p->qn <= le; i++) {
                if (!cui__ed_hit(s, le, i, p->query, p->qn, p->qcase, p->qword)) continue;
                float xa = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, i);
                float xb = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, i + p->qn);
                cui_draw_rect_border(ui, (cui_rect){ x0 + xa - 1, y + 1, xb - xa + 2, p->lh - 2 }, CUI_CORNERS(3), p->match, p->match_edge, 1);
            }
        }
        /* selection: corners rounded only where the selection actually starts
         * or ends, so a multi-line selection joins without scalloped notches */
        if (s1 > s0 && s1 > als && s0 <= ale) {
            int a = CUI_MAX(s0, als) - p->base, b = CUI_MIN(s1, ale) - p->base;
            float xa = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, a);
            float xb = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, b);
            if (s1 > ale) xb += p->cw * 0.6f;   /* the newline is selected too */
            bool top = s0 > als || als == 0;    /* starts on this line */
            bool bot = s1 <= ale + 1;           /* ends on this line */
            cui_corners rad = { top ? 2 : 0, top ? 2 : 0, bot ? 2 : 0, bot ? 2 : 0 };
            cui_draw_rect(ui, (cui_rect){ x0 + xa, y, xb - xa, p->lh }, rad, p->sel);
        }
        /* indent guides: one thin line per tab stop the leading whitespace
         * covers; the innermost hugs the first content column. The caret's
         * line draws its guides in the accent. */
        if (p->guides && le > ls) {
            int wcol = 0, i = ls;
            while (i < le) {
                if (s[i] == '\t') wcol = (wcol / p->tab + 1) * p->tab;
                else if (s[i] == ' ') wcol++;
                else break;
                i++;
            }
            if (wcol >= p->tab) {
                bool on = p->show_line && line == p->caret_line;
                cui_color gc = on ? p->guide_on : p->guide;
                for (int stp = p->tab; stp <= wcol; stp += p->tab)
                    cui_draw_rect(ui, (cui_rect){ x0 + (float)stp * p->cw - 1.0f, y + 1, 1, p->lh - 2 },
                                  CUI_CORNERS(0), gc);
            }
        }
        /* whitespace glyphs: a dot per space, an arrow per tab; the app
         * picks leading-only or all, trailing whitespace draws warm */
        if (p->ws > 0 && le > ls)
            cui__ed_paint_ws(ui, p, s, ls, le, p->ws == 1, x0, y + ty);
        /* the bracket pair */
        for (int m = 0; m < 2; m++)
            if (p->br_line[m] == line)
                cui_draw_rect_border(ui, (cui_rect){ x0 + p->br_px[m] - 0.5f, y + 2, p->cw + 1, p->lh - 4 }, CUI_CORNERS(3),
                                     p->brk, p->brk_edge, 1);
        /* text: tokenized up to the last pixel in view */
        float maxx = (float)(p->vc1 + 1) * p->cw;
        int stop = cui__ed_at_px(ui, p->size, p->cw, p->tab, s, ls, le, maxx);
        if (stop < le) stop = cui__utf8_next(s, le, stop);
        int nn = stop - ls;
        if (nn + 1 > tok_cap) { tok_cap = nn + 1; toks = cui__frame_alloc(ui, sizeof *toks * (size_t)tok_cap); }
        bool was_block = block;
        int nt = cui__tokenize(&p->lang, p->json, s + ls, nn, &block, toks, tok_cap);
        for (int t = 0; t < nt; t++) {
            int a = ls + toks[t].start, b = a + toks[t].len;
            /* With ligatures on, adjacent punctuation tokens draw as one run
             * so a sequence like "->" reaches the shaper as a pair (the
             * tokenizer emits one token per symbol byte). Bundled mono faces
             * keep ligature glyphs at the sum width of their characters, so
             * the caret grid is unchanged. */
#ifdef CUI_TEXT_SHAPING
            if (p->lig && toks[t].kind == TK_PUNCT)
                while (t + 1 < nt && toks[t + 1].kind == TK_PUNCT && ls + toks[t + 1].start == b) {
                    t++;
                    b = ls + toks[t].start + toks[t].len;
                }
#endif
            float xoff = cui__ed_span(ui, p->size, p->cw, p->tab, s, ls, a);
            if (xoff >= maxx) break;
            /* A plain-text token may start with whitespace and still contain text. */
            cui__ed_run(ui, p, s, a, b, xoff, x0, y + ty, p->colors[toks[t].kind]);
        }
        /* a block comment opened past the columns in view still carries on */
        if (stop < le && p->lang.block_comments) block = cui__ed_scan_line(&p->lang, s, ls, le, was_block);
    }
    /* placeholder */
    if (p->empty && p->placeholder)
        cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0, r.y + CUI__ED_INSET + (float)(-p->sy) + ty, p->muted, p->placeholder, -1, true);
    /* caret */
    if (p->focused && p->caret_line >= p->first && p->caret_line < p->first + p->count) {
        float blink = 0.5f + 0.5f * cosf((float)p->caret_phase * 2.0f * CUI_PI / 1.06f);
        float a = (blink > 0.35f || p->caret_phase < 0.5) ? 1.0f : 0.0f;
        float y = r.y + CUI__ED_INSET + (float)((double)p->caret_line * p->lh - p->sy);
        float cx = x0 + p->caret_px;
        cui_draw_rect(ui, (cui_rect){ roundf(cx * ui->scale) / ui->scale - 0.75f, y + ty, 1.5f, fh }, CUI_CORNERS(0.75f), cui_alpha(p->caret, a));
        /* inline suggestion (ghost text) right after the caret: the first
         * line of the suggestion, plus an extra-lines marker when the
         * suggestion spans more than one line (Tab accepts it all) */
        if (p->ghost && p->ghost_len > 0) {
            int fl = p->ghost_len;
            for (int i = 0; i < p->ghost_len; i++)
                if (p->ghost[i] == '\n' || p->ghost[i] == '\r') { fl = i; break; }
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, cx + 3.0f, y + ty,
                                p->muted, p->ghost, fl, true);
            if (p->ghost_extra > 0) {
                char mark[24];
                snprintf(mark, sizeof mark, " \xe2\x80\xa6 +%d", p->ghost_extra);
                float w = cui__text_width(ui, CUI_FONT_MONO, p->size,
                                          p->ghost, fl, true);
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size,
                                    cx + 3.0f + w, y + ty, p->muted, mark, -1, true);
            }
        }
    }
    cui_pop_clip(ui);
    /* scrollbars */
    if (p->max_sy > 0) {
        float at, len, w = p->bar_big == 1 ? 8 : 6;
        cui__ed_thumb(r.h - 6, r.h, p->max_sy, p->sy, &at, &len);
            if (p->nticks > 0 && p->tick_lines) {   /* find hits along the track: always visible, the thumb fades in over them */
                double total = (double)r.h + p->max_sy;
                for (int i = 0; i < p->nticks; i++) {
                    float ty = r.y + 3 + (float)((double)p->tick_lines[i] * p->lh / total) * (r.h - 6);
                    cui_draw_rect(ui, (cui_rect){ r.x + r.w - 7, ty, 4, 2 }, CUI_CORNERS(1), p->tick);
                }
            }
        if (p->bar_t > 0.01f)
            cui_draw_rect(ui, (cui_rect){ r.x + r.w - w - 3, r.y + 3 + at, w, len }, CUI_CORNERS(w * 0.5f),
                          cui_alpha(p->bar, (p->bar_big == 1 ? 0.38f : 0.26f) * p->bar_t));
    }
    if (p->bar_t > 0.01f) {
        if (p->max_sx > 0) {
            float at, len, h = p->bar_big == 2 ? 8 : 6, track = code.w - 6 - (p->max_sy > 0 ? CUI__ED_BAR : 0);
            cui__ed_thumb(track, code.w, p->max_sx, p->sx, &at, &len);
            cui_draw_rect(ui, (cui_rect){ gx + 3 + at, r.y + r.h - h - 3, len, h }, CUI_CORNERS(h * 0.5f),
                          cui_alpha(p->bar, (p->bar_big == 2 ? 0.38f : 0.26f) * p->bar_t));
        }
    }
    cui_pop_clip(ui);
#ifdef CUI_TEXT_SHAPING
    ui->text_ligatures = false;
#endif
    ui->text_unshaped = unshaped;
}

/* ---- the editor ---- */

static cui__ed_state *cui__ed_get(cui_ctx *ui, const char *id_str) {
    return cui_state(ui, cui_make_id(ui, id_str) ^ CUI__ED_SALT, sizeof(cui__ed_state));
}

void cui_code_editor_goto(cui_ctx *ui, const char *id, int line, int column) {
    cui__ed_state *s = cui__ed_get(ui, id);
    s->pend_goto = true;
    s->pend_line = line; s->pend_col = column;
    ui->animating = true;
}

void cui_code_editor_select(cui_ctx *ui, const char *id, int anchor, int cursor) {
    cui__ed_state *s = cui__ed_get(ui, id);
    s->pend_sel = true;
    s->pend_a = anchor; s->pend_c = cursor;
    ui->animating = true;
}

void cui_code_editor_find(cui_ctx *ui, const char *id, const char *query) {
    cui_code_editor_find_opts(ui, id, query, false, false);
}

/* Open the find bar seeded with query; match_case honours letter case. */
void cui_code_editor_find_case(cui_ctx *ui, const char *id, const char *query, bool match_case) {
    cui_code_editor_find_opts(ui, id, query, match_case, false);
}

/* Open the find bar with both toggles set; the bar's Aa and W buttons
 * change them at runtime (read them back with cui_code_editor_find_toggles). */
void cui_code_editor_find_opts(cui_ctx *ui, const char *id, const char *query, bool match_case, bool whole_word) {
    cui__ed_state *s = cui__ed_get(ui, id);
    s->find_case = match_case;
    s->find_word = whole_word;
    if (!query) { s->mode = 0; return; }
    snprintf(s->query, sizeof s->query, "%s", query);
    s->mode = 1;
    s->bar_focus = s->pend_find = true;
    ui->animating = true;
}

/* Current find-bar toggle state (false when no frame has run yet). Lets
 * the app mirror the bar's toggles into its own persisted settings. */
void cui_code_editor_find_toggles(cui_ctx *ui, const char *id, bool *match_case, bool *whole_word) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (match_case) *match_case = s->init && s->find_case;
    if (whole_word) *whole_word = s->init && s->find_word;
}

void cui_code_editor_ghost(cui_ctx *ui, const char *id, const char *text) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (!text || !*text) {
        s->ghost_len = 0;
        return;
    }
    const int cap = 2048;
    if (!s->ghost) {
        s->ghost = (char *)cui__alloc(ui, (size_t)cap + 1);
        if (!s->ghost) return;
    }
    int n = 0;
    while (text[n] && n < cap) n++;
    /* Multi-line suggestions are kept whole: Tab accepts everything and
     * the renderer shows the first line plus an extra-lines marker. */
    memcpy(s->ghost, text, (size_t)n);
    s->ghost[n] = 0;
    s->ghost_len = n;
    s->ghost_cursor = s->ed.cursor;
}

bool cui_code_editor_ghost_active(cui_ctx *ui, const char *id) {
    cui__ed_state *s = cui__ed_get(ui, id);
    return s->init && s->ghost_len > 0;
}

const char *cui_code_editor_ghost_text(cui_ctx *ui, const char *id) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (!s->init || s->ghost_len <= 0) return NULL;
    s->ghost[s->ghost_len] = 0;         /* cap+1 storage keeps this safe */
    return s->ghost;
}

bool cui_code_editor_replace_external(cui_ctx *ui, const char *id,
        char *buf, size_t cap, int len, int a, int b,
        const char *text, int n, int *out_len) {
    if (!ui || !buf || cap == 0 || !text || n < 0) {
        if (out_len) *out_len = len;
        return false;
    }
    if (a < 0) a = 0;
    if (b < a) b = a;
    if (a > len) a = len;
    if (b > len) b = len;
    cui__ed_state *st = cui__ed_get(ui, id);
    if (!st->init) {
        /* No frame has run for this editor yet: create a fresh state without
         * stealing focus, mirroring cui_code_editor_ex's one-time setup. */
        st->init = true;
        st->pref_col = -1;
        cui__state_set_dtor(ui, cui_make_id(ui, id) ^ CUI__ED_SALT, cui__ed_dtor);
    }
    cui__ed E;
    memset(&E, 0, sizeof E);
    E.ui = ui; E.st = st; E.buf = buf; E.cap = cap;
    E.len = len;
    E.rev = 0;                  /* caller bumps its own revision afterwards */
    E.tab = 4;
    E.use_tabs = false;
    if (out_len) *out_len = len;
    if (!cui__ed_replace(&E, a, b, text, n, 3)) return false;
    if (out_len) *out_len = E.len;
    return true;
}

/* how Enter indents: 1 braces (C, C++, JavaScript, TypeScript, Rust, Go,
 * JSON), 2 colons (Python), 0 neither */
static int cui__ed_lang_kind(const char *lang) {
    static const char *const braces[] = { "c", "cpp", "c++", "h", "js", "ts", "javascript", "typescript", "rust", "rs", "go", "json" };
    if (!lang) return 0;
    for (int i = 0; i < CUI_COUNTOF(braces); i++) if (!strcmp(lang, braces[i])) return 1;
    return !strcmp(lang, "python") || !strcmp(lang, "py") ? 2 : 0;
}

static cui_code_editor_result cui__code_editor_ex_body(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o);

/* left to right in any interface: see cui__keep_ltr */
cui_code_editor_result cui_code_editor_ex(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o) {
    int first = ui->node_count;
    cui_code_editor_result r = cui__code_editor_ex_body(ui, id_str, buf, cap, o);
    cui__keep_ltr(ui, first);
    return r;
}

/* One frame of the editor's view, around the editing context: ids, metrics, the box from
 * the last frame and the scroll limits. */
typedef struct {
    cui__ed *E;
    const cui_code_editor_opts *o;
    cui_code_editor_result *res;
    const char *id_str;
    cui_id id, lid, sid, fid, gid;      /* the editor, its lines, its scroll, the find and go to fields */
    int lang_kind;
    float size, lh, cw, gutter, height; /* text size, line height, column width */
    cui_rect br; bool known;            /* the lines' box, last frame (known: it has one) */
    float code_w, gx;                   /* the code's width, and where it starts (right of the gutter) */
    double view_h, content_h, max_sy;
    float max_sx;
    bool over_vbar, over_hbar;          /* the pointer is over a scrollbar's zone */
    int mods; bool shift, prim;
    bool word_mod, line_mod;            /* the platform's word and line modifiers */
    cui_interact lr;
    bool focused;
    int caret_line, caret_col;
    float caret_px;
} cui__edf;

/* requests made from code since the last frame: select, go to, find */
static void cui__edf_requests(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    if (st->pend_sel) {
        e->anchor = CUI_CLAMP(st->pend_a, 0, E->len);
        e->cursor = CUI_CLAMP(st->pend_c, 0, E->len);
        ui->focus = f->lid;
        E->reveal = E->center = true;
        st->pref_col = -1;
    }
    if (st->pend_goto) {
        int line = CUI_CLAMP(st->pend_line - 1, 0, E->nl - 1);
        e->cursor = e->anchor = cui__ed_pos(E, line, (float)CUI_MAX(0, st->pend_col - 1));
        ui->focus = f->lid;
        E->reveal = E->center = true;
        st->pref_col = -1;
    }
    if (st->pend_find) cui__ed_find_go(E, CUI_MIN(e->cursor, e->anchor), 1);
    st->pend_sel = st->pend_goto = st->pend_find = false;
}

/* the find and go to line bar: Ctrl+F and Ctrl+G open it, Escape in it closes it */
static void cui__edf_bar_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool mine = ui->focus == f->lid || ui->focus == f->fid || ui->focus == f->gid;
    if (mine && f->prim && cui_key_pressed(ui, CUI_KEY_F)) {
        int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
        if (s1 > s0 && s1 - s0 < (int)sizeof st->query && !memchr(E->buf + s0, '\n', (size_t)(s1 - s0)))
            snprintf(st->query, sizeof st->query, "%.*s", s1 - s0, E->buf + s0);
        st->mode = 1;
        st->bar_focus = true;
        ui->kpressed[CUI_KEY_F] = 0;
    }
    if (mine && f->prim && cui_key_pressed(ui, CUI_KEY_G)) {
        st->mode = 2;
        st->go[0] = 0;
        st->bar_focus = true;
        ui->kpressed[CUI_KEY_G] = 0;
    }
    if ((ui->focus == f->fid || ui->focus == f->gid) && cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        st->mode = 0;
        ui->focus = f->lid;
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
}

/* a press: on a scrollbar it grabs the thumb; in the gutter it selects lines; in the code it
 * places the caret, and a double or triple click selects a word or a line */
static void cui__edf_press(cui__edf *f, int line, float px) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    st->tab_free = false;
    if (f->over_vbar || f->over_hbar) {
        float at, len;
        if (f->over_vbar) {
            cui__ed_thumb(br.h - 6, br.h, f->max_sy, st->sy, &at, &len);
            float ty = br.y + 3 + at;
            st->bar_grab = ui->my >= ty && ui->my <= ty + len ? ui->my - ty : len * 0.5f;
            st->bar_drag = 1;
        } else {
            float track = br.w - f->gutter - 6 - (f->max_sy > 0 ? CUI__ED_BAR : 0);
            cui__ed_thumb(track, br.w - f->gutter, f->max_sx, st->sx, &at, &len);
            float tx = f->gx + 3 + at;
            st->bar_grab = ui->mx >= tx && ui->mx <= tx + len ? ui->mx - tx : len * 0.5f;
            st->bar_drag = 2;
        }
        return;
    }
    int pos = cui__ed_pos_px(E->ui, f->size, f->cw, E, line,
                             ui->mx < f->gx ? 0.0f : px);
    int clicks = ui->click_count;
    st->drag = ui->mx < f->gx ? 3 : clicks >= 3 ? 3 : clicks == 2 ? 2 : 1;
    if (st->drag == 1) {
        e->cursor = pos;
        if (!f->shift) e->anchor = pos;
        st->drag_a = st->drag_b = e->anchor;
    } else {
        int a, b;
        if (st->drag == 2) cui__ed_word_at(E, pos, &a, &b); else cui__ed_line_at(E, pos, &a, &b);
        st->drag_a = a; st->drag_b = b;
        e->anchor = a; e->cursor = b;
    }
    st->pref_col = -1;
    ui->caret_time = ui->time;
}

/* dragging a scrollbar's thumb */
static void cui__edf_bar_drag(cui__edf *f) {
    cui__ed_state *st = f->E->st;
    cui_ctx *ui = f->E->ui;
    cui_rect br = f->br;
    float at, len;
    if (st->bar_drag == 1) {
        cui__ed_thumb(br.h - 6, br.h, f->max_sy, st->sy, &at, &len);
        float t = (ui->my - br.y - 3 - st->bar_grab) / CUI_MAX(1.0f, br.h - 6 - len);
        st->sy = (double)cui__clampf(t, 0, 1) * f->max_sy;
    } else {
        float track = br.w - f->gutter - 6 - (f->max_sy > 0 ? CUI__ED_BAR : 0);
        cui__ed_thumb(track, br.w - f->gutter, f->max_sx, st->sx, &at, &len);
        float t = (ui->mx - f->gx - 3 - st->bar_grab) / CUI_MAX(1.0f, track - len);
        st->sx = cui__clampf(t, 0, 1) * f->max_sx;
    }
    st->last_activity = ui->time;
}

/* dragging a selection, by characters, words or lines; past an edge the view scrolls,
 * faster further out */
static void cui__edf_drag_select(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    float gx = f->gx, lh = f->lh, cw = f->cw;
    float top = br.y, bottom = br.y + br.h;
    if (ui->my < top || ui->my > bottom) {
        float dist = ui->my < top ? top - ui->my : ui->my - bottom;
        double step = (double)ui->dt * (double)lh * (8.0 + 10.0 * dist / lh);
        st->sy = CUI_MAX(0.0, CUI_MIN(f->max_sy, st->sy + (ui->my < top ? -step : step)));
        ui->animating = true;
    }
    if (st->drag != 3 && (ui->mx < gx || ui->mx > br.x + br.w)) {
        float dist = ui->mx < gx ? gx - ui->mx : ui->mx - (br.x + br.w);
        float step = ui->dt * cw * (12.0f + 1.0f * dist);
        st->sx = cui__clampf(st->sx + (ui->mx < gx ? -step : step), 0, f->max_sx);
        ui->animating = true;
    }
    float y0 = br.y + CUI__ED_INSET - (float)st->sy;
    float x0 = gx + CUI__ED_GAP - st->sx;
    int line = (int)floor(((double)ui->my - y0) / lh);
    float px = ui->mx - x0;
    int pos = cui__ed_pos_px(E->ui, f->size, f->cw, E, line,
                             st->drag == 3 ? 0.0f : px);
    if (line < 0) pos = 0;
    if (line >= E->nl) pos = E->len;
    int prev = e->cursor;
    if (st->drag == 1) e->cursor = pos;
    else {
        int a, b;
        if (st->drag == 2) cui__ed_word_at(E, pos, &a, &b); else cui__ed_line_at(E, pos, &a, &b);
        if (a < st->drag_a) { e->anchor = st->drag_b; e->cursor = a; }
        else { e->anchor = st->drag_a; e->cursor = CUI_MAX(b, st->drag_b); }
    }
    if (e->cursor != prev) { ui->caret_time = ui->time; E->reveal = true; }
}

/* the pointer: press, drags, the wheel */
static void cui__edf_pointer(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    f->lr = cui__interact(ui, f->lid, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    f->gx = br.x + f->gutter;
    float x0 = f->gx + CUI__ED_GAP - st->sx, y0 = br.y + CUI__ED_INSET - (float)st->sy;
    f->over_vbar = f->known && f->max_sy > 0 && ui->mx >= br.x + br.w - CUI__ED_BAR;
    f->max_sx = CUI_MAX(0.0f, (float)CUI_MAX(E->maxlen, 1) * f->cw + CUI__ED_GAP + 24 - f->code_w);
    f->over_hbar = f->known && f->max_sx > 0 && ui->my >= br.y + br.h - CUI__ED_BAR && ui->mx >= f->gx && !f->over_vbar;
    if (f->lr.hovered) cui__set_cursor(ui, (f->over_vbar || f->over_hbar || ui->mx < f->gx) ? CUI_CURSOR_ARROW : CUI_CURSOR_TEXT);
    if (f->known) {
        int line = (int)floor(((double)ui->my - y0) / f->lh);
        float px = ui->mx - x0;
        if (f->lr.hovered && ui->mpressed[0]) cui__edf_press(f, line, px);
        if (st->bar_drag && f->lr.active && ui->mdown[0]) cui__edf_bar_drag(f);
        if (st->drag && f->lr.active && ui->mdown[0] && !ui->mpressed[0]) cui__edf_drag_select(f);
    }
    if (!ui->mdown[0]) st->drag = st->bar_drag = 0;
    if (ui->scroll_target == f->sid && (ui->scroll_dy != 0 || ui->scroll_dx != 0)) {
        float dy = ui->scroll_dy, dx = ui->scroll_dx;
        if (f->shift && dx == 0) { dx = dy; dy = 0; }
        st->sy -= (double)dy * f->lh * 3;
        st->sx -= dx * 44;
        st->last_activity = ui->time;
        ui->scroll_dy = ui->scroll_dx = 0;   /* consumed */
    }
}

/* caret motion: arrows (by character, word or line end), up and down keeping the column,
 * pages, Home, End, select all. Reports the vertical motion. */
static void cui__edf_move_keys(cui__edf *f, int *vmove_out, int *vpage_out) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    const char *buf = E->buf;
    bool shift = f->shift, word_mod = f->word_mod, line_mod = f->line_mod;
    bool whole = (f->mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) != 0;
    int view_lines = CUI_MAX(1, (int)(f->view_h / f->lh) - 1);
    for (int rep = ui->kpressed[CUI_KEY_LEFT]; rep > 0; rep--) {
        int s0 = CUI_MIN(e->cursor, e->anchor);
        if (!shift && e->cursor != e->anchor && !word_mod && !line_mod) { cui__ed_move(E, s0, false); continue; }
        int line = cui__ed_line_of(E, e->cursor);
        cui__ed_move(E, line_mod ? cui__ed_first_text(E, line) : word_mod ? cui__edit_word_left(buf, e->cursor) : cui__utf8_prev(buf, e->cursor), shift);
    }
    for (int rep = ui->kpressed[CUI_KEY_RIGHT]; rep > 0; rep--) {
        int s1 = CUI_MAX(e->cursor, e->anchor);
        if (!shift && e->cursor != e->anchor && !word_mod && !line_mod) { cui__ed_move(E, s1, false); continue; }
        int line = cui__ed_line_of(E, e->cursor);
        cui__ed_move(E, line_mod ? cui__ed_end(E, line) : word_mod ? cui__edit_word_right(buf, E->len, e->cursor) : cui__utf8_next(buf, E->len, e->cursor), shift);
    }
    int vmove = (int)ui->kpressed[CUI_KEY_DOWN] - (int)ui->kpressed[CUI_KEY_UP];
    int vpage = (int)ui->kpressed[CUI_KEY_PAGE_DOWN] - (int)ui->kpressed[CUI_KEY_PAGE_UP];
    if (line_mod && vmove) cui__ed_move(E, vmove < 0 ? 0 : E->len, shift);
    else if (vmove || vpage) {
        int line = cui__ed_line_of(E, e->cursor);
        if (st->pref_col < 0) st->pref_col = cui__ed_caret_col(E, e->cursor);
        int target = line + vmove + vpage * view_lines;
        int keep = st->pref_col;
        if (vpage) st->sy += (double)(CUI_CLAMP(target, 0, E->nl - 1) - line) * f->lh;
        if (target < 0) cui__ed_move(E, 0, shift);
        else if (target >= E->nl) cui__ed_move(E, E->len, shift);
        else cui__ed_move(E, cui__ed_pos(E, target, (float)keep), shift);
        st->pref_col = keep;
    }
    if (cui_key_pressed(ui, CUI_KEY_HOME)) {
        int line = cui__ed_line_of(E, e->cursor), ft = cui__ed_first_text(E, line);
        cui__ed_move(E, whole ? 0 : e->cursor == ft ? E->ls[line] : ft, shift);
    }
    if (cui_key_pressed(ui, CUI_KEY_END)) cui__ed_move(E, whole ? E->len : cui__ed_end(E, cui__ed_line_of(E, e->cursor)), shift);
    if (f->prim && cui_key_pressed(ui, CUI_KEY_A)) { e->anchor = 0; e->cursor = E->len; }
    *vmove_out = vmove; *vpage_out = vpage;
}

/* Backspace: the selection, or back a character, a word, to the line start, or in the
 * indentation to the previous tab stop */
static void cui__edf_backspace(cui__edf *f) {
    cui__ed *E = f->E;
    cui_edit_state *e = &E->st->ed;
    const char *buf = E->buf;
    for (int rep = E->ui->kpressed[CUI_KEY_BACKSPACE]; rep > 0; rep--) {
        int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
        if (s1 > s0) { cui__ed_replace(E, s0, s1, "", 0, 3); continue; }
        if (s0 == 0) continue;
        int line = cui__ed_line_of(E, s0), ls = E->ls[line], to = cui__utf8_prev(buf, s0);
        if (f->word_mod) to = cui__edit_word_left(buf, s0);
        else if (f->line_mod) to = ls;
        else if (s0 > ls && cui__ed_first_text(E, line) >= s0 && buf[s0 - 1] == ' ') {
            int col = cui__ed_col(buf, ls, s0, E->tab), stop = (col - 1) / E->tab * E->tab;
            to = s0;
            while (to > ls && buf[to - 1] == ' ' && cui__ed_col(buf, ls, to - 1, E->tab) >= stop) to--;
        }
        cui__ed_replace(E, to, s0, "", 0, 2);
    }
}

/* editing keys (not read only): paste, undo and redo, Backspace, Delete, Enter, Tab, typing */
static void cui__edf_edit_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    char *buf = E->buf;
    bool shift = f->shift, prim = f->prim;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (prim && cui_key_pressed(ui, CUI_KEY_V) && ui->platform.get_clipboard) {
        const char *clip = ui->platform.get_clipboard(ui->platform.user);
        if (clip && *clip) {
            cui__ed_replace(E, s0, s1, clip, (int)strlen(clip), 3);
            e->last_edit = 0;   /* typing after a paste is its own step */
        }
    }
    bool undo = prim && !shift && cui_key_pressed(ui, CUI_KEY_Z);
    bool redo = prim && ((shift && cui_key_pressed(ui, CUI_KEY_Z)) || cui_key_pressed(ui, CUI_KEY_Y));
    if ((undo || redo) && cui__ed_hist_step(E, !undo)) {
        E->changed = E->reveal = true;
        st->pref_col = -1;
    }
    cui__edf_backspace(f);
    for (int rep = ui->kpressed[CUI_KEY_DELETE]; rep > 0; rep--) {
        s0 = CUI_MIN(e->cursor, e->anchor); s1 = CUI_MAX(e->cursor, e->anchor);
        if (s1 > s0) { cui__ed_replace(E, s0, s1, "", 0, 3); continue; }
        if (s0 >= E->len) continue;
        int to = f->word_mod ? cui__edit_word_right(buf, E->len, s0) : cui__utf8_next(buf, E->len, s0);
        cui__ed_replace(E, s0, to, "", 0, 2);
        e->cursor = e->anchor = e->last_cursor = s0;
    }
    for (int rep = ui->kpressed[CUI_KEY_ENTER]; rep > 0 && !prim; rep--) cui__ed_newline(E, f->lang_kind);
    if (cui_key_pressed(ui, CUI_KEY_TAB) && !(f->mods & CUI_MOD_CTRL)) {
        s0 = CUI_MIN(e->cursor, e->anchor); s1 = CUI_MAX(e->cursor, e->anchor);
        if (st->ghost_len > 0 && s1 == s0) {
            /* accept the inline suggestion as one undoable edit */
            cui__ed_replace(E, s0, s0, st->ghost, st->ghost_len, 1);
        } else if (shift || s1 > s0) cui__ed_indent(E, shift);
        else {
            int col = cui__ed_caret_col(E, s0), n = E->use_tabs ? 1 : E->tab - col % E->tab;
            char sp[17];
            memset(sp, E->use_tabs ? '\t' : ' ', sizeof sp);
            cui__ed_replace(E, s0, s0, sp, n, 3);
        }
    }
    if (ui->text_in_len > 0 && !(prim && !(f->mods & CUI_MOD_ALT))) cui__ed_type(E, ui->text_in, ui->text_in_len, f->lang_kind);
}

/* the keyboard while the lines have focus */
static void cui__edf_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    ui->edit = e;   /* cui_wants_keyboard, caret blink wakeups */
#if defined(__APPLE__)
    f->word_mod = (f->mods & CUI_MOD_ALT) != 0; f->line_mod = (f->mods & CUI_MOD_SUPER) != 0;
#else
    f->word_mod = (f->mods & CUI_MOD_CTRL) != 0; f->line_mod = false;
#endif
    int before_c = e->cursor, before_a = e->anchor;
    for (int k = CUI_KEY_NONE + 1; k < CUI_KEY_COUNT; k++)
        if (ui->kpressed[k] && k != CUI_KEY_TAB && k != CUI_KEY_ESCAPE) st->tab_free = false;
    if (ui->text_in_len > 0) st->tab_free = false;
    int vmove, vpage;
    cui__edf_move_keys(f, &vmove, &vpage);
    /* clipboard: without a selection, copy and cut take the whole line */
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (f->prim && (cui_key_pressed(ui, CUI_KEY_C) || cui_key_pressed(ui, CUI_KEY_X))) {
        int a = s0, b = s1;
        if (a == b) cui__ed_line_at(E, a, &a, &b);
        if (b > a && ui->platform.set_clipboard) ui->platform.set_clipboard(ui->platform.user, cui__frame_strdup(ui, E->buf + a, b - a));
        if (cui_key_pressed(ui, CUI_KEY_X) && b > a) cui__ed_replace(E, a, b, "", 0, 3);
    }
    if (!E->ro) cui__edf_edit_keys(f);
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->ghost_len > 0) cui__ed_ghost_clear(st);
        else if (st->mode) st->mode = 0;
        else if (e->cursor != e->anchor) e->anchor = e->cursor;
        else st->tab_free = true;   /* the next Tab leaves the editor */
    }
    if (st->ghost_len > 0 && e->cursor != st->ghost_cursor) cui__ed_ghost_clear(st);
    if (e->cursor != before_c || e->anchor != before_a || E->changed) {
        ui->caret_time = ui->time;
        if (vmove == 0 && vpage == 0) st->pref_col = -1;
    }
    if (!E->ro && !st->tab_free) ui->tab_owner = f->lid;
}

/* the find bar's field, the match count and the previous and next buttons */
static void cui__edf_build_find(cui__edf *f, cui_input_opts *io, cui_button_opts *bo, bool narrow) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool shift = f->shift;
    if (st->bar_focus) { ui->focus = f->fid; ui->tf_select_all = f->fid; st->bar_focus = false; }
    io->placeholder = "Find";
    io->icon = narrow ? NULL : CUI_ICON_SEARCH;
    int fl = cui_input_ex(ui, "find", st->query, sizeof st->query, io);
    int qn = (int)strlen(st->query);
    int s0 = CUI_MIN(e->cursor, e->anchor);
    if (fl & CUI_CHANGED) cui__ed_find_go(E, s0, 1);   /* as you type, from where the selection starts */
    if (fl & CUI_SUBMITTED) cui__ed_find_go(E, shift ? s0 : s0 + (e->cursor != e->anchor ? 1 : 0), shift ? -1 : 1);
    /* case / whole-word toggles: flipping either re-runs the search from
     * the selection so the count and the highlight catch up immediately */
    bo->tooltip = "Match case";
    bo->selected = st->find_case;
    bo->color = st->find_case ? CUI_COLOR_ACCENT : (cui_color){ 0 };
    if (cui_button_ex(ui, "Aa", bo)) { st->find_case = !st->find_case; cui__ed_find_go(E, s0, 1); }
    ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->tooltip = "Match whole word";
    bo->selected = st->find_word;
    bo->color = st->find_word ? CUI_COLOR_ACCENT : (cui_color){ 0 };
    if (cui_button_ex(ui, "W", bo)) { st->find_word = !st->find_word; cui__ed_find_go(E, s0, 1); }
    ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->selected = false;
    bo->color = (cui_color){ 0 };
    bo->tooltip = NULL;
    /* the count, and which match the selection is */
    int total = 0, index = 0;
    s0 = CUI_MIN(e->cursor, e->anchor);
    int s1 = CUI_MAX(e->cursor, e->anchor);
    bool on_match = qn > 0 && s1 - s0 == qn && cui__ed_hit(E->buf, E->len, s0, st->query, qn, st->find_case, st->find_word);
    for (int i = 0; qn > 0 && i + qn <= E->len; i++)
        if (cui__ed_hit(E->buf, E->len, i, st->query, qn, st->find_case, st->find_word)) { total++; if (i < s0 || (on_match && i == s0)) index++; }
    f->res->find_matches = total;
    f->res->find_index = on_match ? index : 0;
    const char *label = qn == 0 ? "" : narrow ? (on_match ? cui__frame_printf(ui, "%d/%d", index, total) : cui__frame_printf(ui, "%d", total))
                      : total == 0 ? "No results" : on_match ? cui__frame_printf(ui, "%d of %d", index, total)
                      : cui__frame_printf(ui, "%d match%s", total, total == 1 ? "" : "es");
    if (*label) {
        int t = cui__text_node(ui, label, -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), total ? CUI_C(ui, TEXT_SECONDARY) : CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[t].flags |= CUI_NF_TABULAR;
        ui->nodes[t].line_height = 1.3f;
        ui->nodes[t].pad[1] = ui->nodes[t].pad[3] = 4;
    }
    bo->disabled = total == 0;
    bo->tooltip = "Previous match"; bo->shortcut = "Shift Enter";
    if (cui_icon_button_ex(ui, CUI_ICON_CHEVRON_UP, bo)) cui__ed_find_go(E, s0, -1);
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->tooltip = "Next match"; bo->shortcut = "Enter";
    if (cui_icon_button_ex(ui, CUI_ICON_CHEVRON_DOWN, bo)) cui__ed_find_go(E, s0 + (s1 > s0 ? 1 : 0), 1);
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->disabled = false;
}

/* the go to line field: "line" or "line:column" */
static void cui__edf_build_goto(cui__edf *f, cui_input_opts *io, bool narrow) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    if (st->bar_focus) { ui->focus = f->gid; ui->tf_select_all = f->gid; st->bar_focus = false; }
    io->placeholder = narrow ? "Go to line" : cui__frame_printf(ui, "Go to line, 1 to %d", E->nl);
    io->icon = narrow ? NULL : CUI_ICON_ARROW_DOWN;
    int fl = cui_input_ex(ui, "goto", st->go, sizeof st->go, io);
    if (!(fl & CUI_SUBMITTED)) return;
    char *end = NULL;
    long line = strtol(st->go, &end, 10), col = 1;
    if (end && *end == ':') col = strtol(end + 1, NULL, 10);
    if (end != st->go) {
        line = CUI_CLAMP(line, 1, E->nl);
        e->cursor = e->anchor = cui__ed_pos(E, (int)line - 1, (float)CUI_MAX(0, col - 1));
        E->reveal = E->center = true;
        st->pref_col = -1;
        st->mode = 0;
        ui->focus = f->lid;
    }
}

/* the bar above the lines: find or go to line, and a close button */
static void cui__edf_build_bar(cui__edf *f) {
    cui__ed_state *st = f->E->st;
    cui_ctx *ui = f->E->ui;
    bool narrow = f->known && f->br.w < 360;   /* a phone: a shorter count, no search icon */
    int bar = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[bar];
    bn->dir = CUI_ROW; bn->align = CUI_ALIGN_CENTER; bn->gap = 4;
    bn->w = CUI_FILL;
    bn->pad[0] = bn->pad[2] = 6; bn->pad[1] = 6; bn->pad[3] = 6;
    bn->bg = CUI_C(ui, SURFACE);
    cui__open(ui, bar);
    cui_push_id(ui, f->id_str);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM;
    io.width = CUI_FILL;
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM;
    bo.variant = CUI_BUTTON_GHOST;
    if (st->mode == 1) cui__edf_build_find(f, &io, &bo, narrow);
    else cui__edf_build_goto(f, &io, narrow);
    bo.tooltip = "Close"; bo.shortcut = "Esc";
    if (cui_icon_button_ex(ui, CUI_ICON_X, &bo)) { st->mode = 0; ui->focus = f->lid; }
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    cui_pop_id(ui);
    cui__close(ui);
    int hl = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
    ui->nodes[hl].bg = CUI_C(ui, BORDER);
}

/* follow the caret, then keep the scroll in range */
static void cui__edf_follow_caret(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    float cw = f->cw, lh = f->lh, code_w = f->code_w;
    double view_h = f->view_h;
    if (E->changed) {
        f->content_h = (double)E->nl * lh;
        f->max_sy = CUI_MAX(0.0, f->content_h - view_h);
        f->max_sx = CUI_MAX(0.0f, (float)CUI_MAX(E->maxlen, 1) * cw + CUI__ED_GAP + 24 - code_w);
    }
    f->caret_line = cui__ed_line_of(E, e->cursor);
    f->caret_col = cui__ed_col(E->buf, E->ls[f->caret_line], e->cursor, E->tab);
    f->caret_px = cui__ed_span(E->ui, f->size, cw, E->tab, E->buf,
                               E->ls[f->caret_line], e->cursor);
    /* a line wider than the longest in bytes (tabs) still scrolls into reach */
    f->max_sx = CUI_MAX(f->max_sx, f->caret_px + CUI__ED_GAP + 24 - code_w);
    if (E->reveal) {
        double cy = (double)f->caret_line * lh;
        bool out = cy < st->sy || cy + lh > st->sy + view_h;
        if (E->center && out) {   /* whole lines: none cut at the top */
            double k = floor((cy - view_h * 0.5) / lh + 0.5);
            st->sy = k > 0 ? k * lh + CUI__ED_INSET : 0;
        }
        else if (cy < st->sy) st->sy = cy;
        else if (cy + lh > st->sy + view_h) st->sy = cy + lh - view_h;
        float cx = f->caret_px, margin = CUI_MIN(cw * 4, code_w * 0.25f);
        if (cx < st->sx + margin) st->sx = cx - margin;
        if (cx > st->sx + code_w - margin) st->sx = cx - code_w + margin;
        if (E->center && (cx < st->sx || cx > st->sx + code_w)) st->sx = cx - code_w * 0.5f;
    }
    st->sy = CUI_MAX(0.0, CUI_MIN(f->max_sy, st->sy));
    st->sx = cui__clampf(st->sx, 0, f->max_sx);
}

/* scrollbar hit markers for the find query: cached line numbers, rebuilt when
 * the query, its toggles or the text changes. The scan is capped to
 * CUI__ED_TICK_SCAN bytes and CUI__ED_TICK_MAX hits, so a find on a huge file
 * or a very common query never stalls a frame; past a cap the ticks are
 * simply left out. */
#define CUI__ED_TICK_SCAN (2u << 20)   /* 2 MB of text scanned at most */
#define CUI__ED_TICK_MAX  1024
static void cui__edf_ticks(cui__ed *E) {
    cui__ed_state *st = E->st;
    if (st->mode != 1 || !st->query[0]) { st->tk_valid = false; st->tk_n = 0; return; }
    if (st->tk_valid && st->tk_rev == E->rev && st->tk_len == E->len &&
        st->tk_case == st->find_case && st->tk_word == st->find_word &&
        strcmp(st->tk_query, st->query) == 0) return;   /* cache is current */
    st->tk_valid = true;
    st->tk_rev = E->rev; st->tk_len = E->len;
    st->tk_case = st->find_case; st->tk_word = st->find_word;
    snprintf(st->tk_query, sizeof st->tk_query, "%s", st->query);
    st->tk_n = 0;
    int qn = (int)strlen(st->query);
    if (qn <= 0 || E->len > (int)CUI__ED_TICK_SCAN) return;
    if (st->tk_cap == 0) {
        st->tk_cap = CUI__ED_TICK_MAX;
        st->tk_line = (int *)cui__alloc(E->ui, sizeof(int) * (size_t)st->tk_cap);
    }
    int line = 0;
    const char *s = E->buf;
    for (int i = 0; i + qn <= E->len; i++) {
        while (line + 1 < E->nl && E->ls[line + 1] <= i) line++;   /* the line i sits on */
        if (cui__ed_hit(s, E->len, i, st->query, qn, st->find_case, st->find_word)) {
            st->tk_line[st->tk_n++] = line;
            if (st->tk_n >= CUI__ED_TICK_MAX) break;
            i += qn - 1;
        }
    }
}

/* the editor's lines: one node whose paint draws the lines in view, copied into frame memory */
static void cui__edf_build_lines(cui__edf *f, const cui__lang *L, bool json, bool dark) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    float lh = f->lh, cw = f->cw;
    bool focused = f->focused;
    int body = cui__node_new(ui, f->lid);
    cui_node *bdn = &ui->nodes[body];
    bdn->w = CUI_FILL; bdn->h = CUI_FILL;
    cui__mark_interactive(ui, body, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui__ed_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->sid = f->sid;
    int first = (int)CUI_MAX(0.0, floor((st->sy - CUI__ED_INSET) / lh));
    int last = (int)CUI_MIN((double)E->nl, ceil((st->sy + f->view_h + 2 * CUI__ED_INSET) / lh) + 1);
    if (first > last) first = last;
    cui__edf_ticks(E);   /* scrollbar hit markers for the find query */
    p->first = first; p->count = last - first; p->nl = E->nl;
    p->tick_lines = st->tk_line; p->nticks = st->tk_n;   /* lives as long as the editor state */
    int a = E->ls[first], b = last > first ? cui__ed_end(E, last - 1) : a;
    p->base = a;
    p->text = cui__frame_strdup(ui, E->buf + a, b - a);   /* the caller may change the buffer before painting */
    int *ls = cui__frame_alloc(ui, sizeof(int) * (size_t)(p->count + 1));
    memcpy(ls, E->ls + first, sizeof(int) * (size_t)(p->count + 1));
    p->ls = ls;
    p->lang = *L; p->json = json;
    p->in_block = cui__ed_block_state(E, L, first);
    p->tab = E->tab;
    p->cursor = e->cursor; p->anchor = e->anchor;
    p->caret_line = f->caret_line; p->caret_col = f->caret_col;
    p->caret_px = f->caret_px;
    p->focused = focused;
    p->show_line = focused || ui->focus == f->fid || ui->focus == f->gid;
    p->ro = E->ro;
    p->empty = E->len == 0;
    p->guides = !f->o->no_indent_guides;
    p->ws = f->o->show_whitespace;
    p->placeholder = f->o->placeholder;
    p->lig = f->o->ligatures;
    p->size = f->size; p->lh = lh; p->cw = cw; p->gutter = f->gutter;
    p->view_h = f->known ? f->br.h : 0;
    p->sy = st->sy; p->max_sy = f->max_sy;
    p->sx = st->sx; p->max_sx = f->max_sx;
    p->vc0 = (int)floorf(st->sx / cw) - 1;
    p->vc1 = (int)ceilf((st->sx + f->code_w + CUI__ED_GAP) / cw) + 1;
    p->caret_phase = ui->time - ui->caret_time;
    p->ghost = st->ghost_len > 0 ? st->ghost : NULL;
    p->ghost_len = st->ghost_len;
    p->ghost_extra = 0;
    if (p->ghost) {
        int lines = 1;
        for (int i = 0; i < st->ghost_len; i++)
            if (st->ghost[i] == '\n') lines++;
        p->ghost_extra = lines - 1;
    }
    p->br_line[0] = p->br_line[1] = -1;
    if (e->cursor == e->anchor && p->show_line) {
        int ba, bb;
        cui__ed_brackets(E, e->cursor, &ba, &bb);
        if (ba >= 0) {
            f->res->bracket = ba; f->res->bracket_match = bb;
            int pos[2] = { ba, bb };
            for (int m = 0; m < 2; m++) {
                p->br_line[m] = cui__ed_line_of(E, pos[m]);
                p->br_px[m] = cui__ed_span(ui, f->size, cw, E->tab, E->buf,
                                            E->ls[p->br_line[m]], pos[m]);
            }
        }
    }
    if (st->mode == 1 && st->query[0]) { p->query = cui__frame_strdup(ui, st->query, -1); p->qn = (int)strlen(p->query); p->qcase = st->find_case; p->qword = st->find_word; }
    if (f->o->marks && f->o->mark_count > 0) {   /* the marks that reach the lines in view, copied: the caller owns them */
        const cui_text_mark *mk = f->o->marks;
        int n = f->o->mark_count, lo = 0, hi = n;
        while (lo < hi) { int m = lo + (hi - lo) / 2; if (mk[m].start < a - 4096) lo = m + 1; else hi = m; }   /* starts are sorted; marks are short */
        int k0 = lo, k1 = lo;
        while (k1 < n && mk[k1].start <= b) k1++;
        int c = 0;
        cui_text_mark *keep = cui__frame_alloc(ui, sizeof *keep * (size_t)CUI_MAX(1, k1 - k0));
        for (int k = k0; k < k1; k++) if (mk[k].end >= a) keep[c++] = mk[k];
        p->marks = keep; p->nmarks = c;
        for (int k = 0; k < 7; k++) {
            cui_color base = k == 0 ? (ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT)) : ui->theme.colors[CUI_TOKEN_SERIES_1 + (k - 1)];
            p->mark_colors[k] = cui_alpha(base, dark ? 0.34f : 0.26f);
        }
    }
    cui__syntax_colors(ui, p->colors);
    cui_color fg = CUI_C(ui, FOREGROUND);
    cui_color acc = ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT);
    /* VS Code behaviour: an unfocused editor dims the whole gutter, easing
     * with the same focus tween the border uses (st->focus_t is updated
     * before this call every frame). */
    p->num = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.45f + 0.25f * st->focus_t);
    p->num_on = cui_alpha(CUI_C(ui, TEXT_SECONDARY), 0.5f + 0.5f * st->focus_t);
    p->band = cui_alpha(fg, dark ? 0.05f : 0.04f);
    p->sel = cui_alpha(acc, ui->accent == CUI_ACCENT_NEUTRAL ? 0.28f : 0.26f);
    if (!focused && !p->show_line) p->sel = cui_alpha(fg, dark ? 0.12f : 0.08f);   /* resting: a quiet selection */
    p->match = cui_alpha(acc, 0.12f);
    p->match_edge = cui_alpha(acc, dark ? 0.55f : 0.45f);
    p->tick = cui_alpha(acc, dark ? 0.8f : 0.7f);   /* scrollbar hit markers: solid enough to read at 2px */
    p->brk = cui_alpha(fg, dark ? 0.10f : 0.06f);
    p->brk_edge = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.55f);
    p->caret = ui->accent == CUI_ACCENT_NEUTRAL ? fg : CUI_C(ui, ACCENT);
    p->muted = CUI_C(ui, TEXT_MUTED);
    p->line = CUI_C(ui, BORDER);
    p->guide = cui_alpha(fg, dark ? 0.11f : 0.08f);
    p->guide_on = cui_alpha(acc, dark ? 0.45f : 0.38f);
    p->ws_glyph = cui_alpha(fg, dark ? 0.30f : 0.24f);
    p->ws_end = cui_alpha(cui_oklch(0.62f, 0.16f, 35, 1), dark ? 0.45f : 0.40f);
    p->bar = ui->accent == CUI_ACCENT_NEUTRAL ? fg : CUI_C(ui, ACCENT);
    /* the scrollbars show while scrolling, while hovered or held */
    bool recent = st->last_activity > 0 && ui->time - st->last_activity < 0.9;
    bool vb = f->over_vbar && (f->lr.hovered || st->bar_drag == 1), hb = f->over_hbar && (f->lr.hovered || st->bar_drag == 2);
    bool show_bar = recent || vb || hb || st->bar_drag || (f->lr.hovered && (f->max_sy > 0 || f->max_sx > 0));
    if (recent) cui__wakeup_after(ui, (float)(0.9 - (ui->time - st->last_activity)) + 0.01f);
    cui__tween(ui, &st->bar_t, show_bar ? 1.0f : 0.0f, show_bar ? 0.12f : 0.3f);
    p->bar_t = st->bar_t;
    p->bar_big = (vb || st->bar_drag == 1) ? 1 : (hb || st->bar_drag == 2) ? 2 : 0;
    bdn->paint = cui__paint_editor;
    bdn->paint_data = p;
}

static cui_code_editor_result cui__code_editor_ex_body(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o) {
    cui_code_editor_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_code_editor_result res;
    memset(&res, 0, sizeof res);
    res.bracket = res.bracket_match = -1;
    static char empty[1];
    if (!buf || cap == 0) { buf = empty; cap = 1; empty[0] = 0; }
    bool trusted = o->length > 0 && (size_t)o->length < cap && buf[o->length] == 0;
    if (!trusted && !memchr(buf, 0, cap)) buf[cap - 1] = 0;   /* never read past the capacity */

    cui__edf f;
    memset(&f, 0, sizeof f);
    f.o = o; f.res = &res; f.id_str = id_str;
    f.id = cui_make_id(ui, id_str);
    f.lid = f.id ^ 0xED1B0D1u; f.sid = f.id ^ 0xED15C011u;
    cui__ed_state *st = cui_state(ui, f.id ^ CUI__ED_SALT, sizeof *st);
    cui__edit_attach(ui, f.id ^ CUI__ED_SALT, &st->ed);
    if (!st->init) {
        st->init = true;
        st->pref_col = -1;
        cui__state_set_dtor(ui, f.id ^ CUI__ED_SALT, cui__ed_dtor);
        if (o->autofocus) ui->focus = f.lid;
    }
    cui_push_id(ui, id_str);
    f.fid = cui_make_id(ui, "find");
    f.gid = cui_make_id(ui, "goto");
    cui_pop_id(ui);

    cui__ed E;
    memset(&E, 0, sizeof E);
    E.ui = ui; E.st = st; E.buf = buf; E.cap = cap;
    E.len = trusted ? o->length : (int)strlen(buf);
    E.rev = o->revision;
    E.tab = o->tab_size > 0 ? CUI_MIN(o->tab_size, 16) : 4;
    E.use_tabs = o->use_tabs;
    E.ro = o->read_only;
    cui__ed_index(&E, false);
    f.E = &E;
    cui_edit_state *e = &st->ed;
    e->cursor = CUI_CLAMP(e->cursor, 0, E.len);
    e->anchor = CUI_CLAMP(e->anchor, 0, E.len);
    st->drag_a = CUI_CLAMP(st->drag_a, 0, E.len);   /* the text may change during a drag */
    st->drag_b = CUI_CLAMP(st->drag_b, 0, E.len);
    f.lang_kind = cui__ed_lang_kind(o->lang);
    bool json = o->lang && !strcmp(o->lang, "json");
    cui__lang L = cui__lang_for(json ? NULL : o->lang);

    f.size = cui__fs(ui, o->size > 0 ? o->size : 13);
    f.lh = roundf(f.size * 1.6f);
    f.cw = cui__text_width(ui, CUI_FONT_MONO, f.size, "0000000000000000", 16, true) / 16.0f;
    int digits = 2;
    for (int v = E.nl / 100; v > 0; v /= 10) digits++;
    f.gutter = o->no_line_numbers ? CUI__ED_GAP : 12 + (float)digits * f.cw + 14;
    f.height = o->height > 0 ? o->height : 320;

    cui__edf_requests(&f);

    f.br = cui__prev_rect(ui, f.lid);
    f.known = cui__has_prev_rect(ui, f.lid) && f.br.w > 0;
    if (!f.known) { ui->animating = true; f.br.w = 480; f.br.h = f.height - 2 - (st->mode ? 41 : 0); }
    f.code_w = CUI_MAX(1.0f, f.br.w - f.gutter - CUI__ED_GAP);
    f.view_h = CUI_MAX((double)f.lh, (double)f.br.h - 2 * CUI__ED_INSET);
    f.content_h = (double)E.nl * f.lh;
    f.max_sy = CUI_MAX(0.0, f.content_h - f.view_h);

    /* input */
    f.mods = ui->mods;
    f.shift = (f.mods & CUI_MOD_SHIFT) != 0; f.prim = (f.mods & CUI_MOD_PRIMARY) != 0;
    cui__edf_bar_keys(&f);
    cui__edf_pointer(&f);
    f.focused = ui->focus == f.lid;
    if (!f.focused) st->tab_free = false;
    if (f.focused) cui__edf_keys(&f);

    /* build */
    bool dark = ui->mode == CUI_THEME_DARK;
    int outer = cui__node_new(ui, f.id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(f.height);
    on->min_h = 80;
    on->radius = CUI_CORNERS(ui->theme.radius_control);
    cui_color bg = dark ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, BACKGROUND), 0.4f) : CUI_C(ui, SURFACE_MUTED);
    on->bg = bg;   /* the code block's surface */
    on->border = 1;
    cui__tween(ui, &st->focus_t, (ui->focus == f.lid || ui->focus == f.fid || ui->focus == f.gid) ? 1.0f : 0.0f, CUI__T_FOCUS);
    on->border_color = cui_mix(CUI_C(ui, BORDER), cui__edge_on(ui), st->focus_t * 0.7f);
    on->flags |= CUI_NF_CLIP;
    cui__open(ui, outer);
    if (st->mode) cui__edf_build_bar(&f);
    cui__edf_follow_caret(&f);
    cui__edf_build_lines(&f, &L, json, dark);
    if (f.focused) cui__wakeup_after(ui, 0.53f);
    if (f.focused && ui->platform.set_text_input_rect) {
        cui_rect cr = { f.gx + CUI__ED_GAP - st->sx + f.caret_px, f.br.y + CUI__ED_INSET + (float)((double)f.caret_line * f.lh - st->sy), 1, f.lh };
        ui->platform.set_text_input_rect(ui->platform.user, cr);
    }
    if (f.focused) cui__focus_ring(ui, outer, ui->focus_visible ? 1.0f : 0.0f);
    cui__close(ui);   /* outer */
    ui->last_id = f.id;

    res.changed = E.changed;
    res.refused = E.refused;
    res.focused = ui->focus == f.lid || ui->focus == f.fid || ui->focus == f.gid;
    res.cursor = e->cursor; res.anchor = e->anchor;
    res.line = f.caret_line + 1; res.column = f.caret_col + 1;
    res.line_count = E.nl;
    res.length = E.len;
    return res;
}
