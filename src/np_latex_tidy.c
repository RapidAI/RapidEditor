#include "np_latex_tidy.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Environments whose contents must be left byte-for-byte untouched. */
static const char *const PROTECTED_ENVS[] = {
    "verbatim", "verbatim*", "lstlisting", "minted", "Verbatim",
    "BVerbatim", "LVerbatim", "comment", "alltt", "tikzpicture", "tikzcd",
    "asy", "gnuplot", "luacode", "explsyntaxon", NULL
};
/* Display-math environments: the content is protected from hard-wrap joining
 * and indentation changes too (they are whitespace-sensitive to authors). */
static const char *const MATH_ENVS[] = {
    "equation", "equation*", "align", "align*", "alignat", "alignat*",
    "gather", "gather*", "multline", "multline*", "flalign", "flalign*",
    "eqnarray", "eqnarray*", "displaymath", "split", "aligned", "cases",
    "matrix", "pmatrix", "bmatrix", "vmatrix", "Vmatrix", "smallmatrix", NULL
};

static bool in_list(const char *name, const char *const *list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(name, list[i]) == 0) return true;
    return false;
}

/* Dynamic output buffer. */
typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

static bool buf_reserve(buf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + need + 1) nc *= 2;
    char *p = (char *)realloc(b->p, nc);
    if (!p) return false;
    b->p = p; b->cap = nc;
    return true;
}
static bool buf_add(buf_t *b, const char *s, size_t n) {
    if (!buf_reserve(b, n)) return false;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return true;
}
static bool buf_addc(buf_t *b, char c) { return buf_add(b, &c, 1); }

/* Match "\begin{name}" or "\end{name}" at the start of a (trimmed) line.
 * Returns 1=begin, -1=end, 0=none; copies the name into name[]. */
static int env_command(const char *s, char *name, size_t cap) {
    if (s[0] != '\\') return 0;
    const char *kw = NULL;
    int sign = 0;
    if (!strncmp(s, "\\begin{", 7)) { kw = s + 7; sign = 1; }
    else if (!strncmp(s, "\\end{", 5)) { kw = s + 5; sign = -1; }
    if (!kw) return 0;
    const char *close = strchr(kw, '}');
    if (!close) return 0;
    size_t n = (size_t)(close - kw);
    if (n == 0 || n >= cap) return 0;
    memcpy(name, kw, n); name[n] = 0;
    return sign;
}

/* Is this line (after leading whitespace) a comment line? A leading % that is
 * not escaped starts a comment; "\%" is a literal percent and not a comment. */
static bool is_comment_line(const char *s) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '%') return false;
    /* A line beginning "%" is a comment even if it also has code after. */
    return true;
}

/* Leading-whitespace count. */
static size_t lead_ws(const char *s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

/* Trim trailing spaces/tabs (does not touch the newline, which is separate). */
static size_t trim_end(const char *s, size_t n) {
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return n;
}

/* Whether a line is a "soft" text line that may be joined with the next one:
 * not blank, not a comment, not a TeX command/env opener, no display-math
 * delimiters, and no CJK-safe inline math. */
static bool joinable(const char *s, size_t n) {
    size_t a = lead_ws(s), b = trim_end(s, n);
    if (a >= b) return false;
    const char *p = s + a;
    if (*p == '%' || *p == '\\' || *p == '$') return false;
    /* Inline $...$ anywhere makes joining risky for math spans across lines. */
    for (size_t i = a; i < b; i++) if (s[i] == '$') return false;
    return true;
}

int np_latex_tidy(const char *src, size_t n, const np_tidy_opts *opts, char **out) {
    if (!out) return -1;
    *out = NULL;
    np_tidy_opts def;
    memset(&def, 0, sizeof def);
    def.normalize_indent = true;
    def.join_hard_wraps = true;
    def.trim_trailing_ws = true;
    def.indent_spaces = 4;
    if (!opts) opts = &def;

    /* Split into lines (without the '\n'), keeping track of a trailing newline. */
    size_t cap = n ? n + 1 : 1;
    char *copy = (char *)malloc(cap + 1);
    if (!copy) return -1;
    memcpy(copy, src, n); copy[n] = 0;
    size_t *offs = NULL, *lens = NULL;
    size_t nlines = 0, lcap = 0;
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && src[i] != '\n') i++;
        if (nlines >= lcap) {
            lcap = lcap ? lcap * 2 : 128;
            size_t *o2 = (size_t *)realloc(offs, lcap * sizeof(size_t));
            if (!o2) { free(offs); free(lens); free(copy); return -1; }
            offs = o2;
            size_t *l2 = (size_t *)realloc(lens, lcap * sizeof(size_t));
            if (!l2) { free(offs); free(lens); free(copy); return -1; }
            lens = l2;
        }
        size_t len = i - start;
        if (len && copy[start + len - 1] == '\r') len--;
        offs[nlines] = start; lens[nlines] = len; nlines++;
        if (i < n && src[i] == '\n') i++;
    }
    bool trailing_nl = n > 0 && src[n - 1] == '\n';

    buf_t b; memset(&b, 0, sizeof b);
    int depth = 0;                 /* \begin nesting for indentation */
    int protect = 0;               /* >0 while inside a protected/math env */
    char stack[128][64];
    int stack_n = 0;

    for (size_t li = 0; li < nlines; li++) {
        const char *line = src + offs[li];
        size_t llen = lens[li];
        /* Make a NUL-terminated view for parsing commands. */
        memcpy(copy, line, llen); copy[llen] = 0;
        char *t = copy;
        while (*t == ' ' || *t == '\t') t++;
        char name[64];
        int ec = env_command(t, name, sizeof name);

        bool comment = is_comment_line(t);
        bool is_prot = in_list(name, PROTECTED_ENVS) || in_list(name, MATH_ENVS);
        bool raw = (protect > 0) || comment;  /* keep byte-for-byte */

        if (ec > 0) {
            if (stack_n < 128) snprintf(stack[stack_n++], sizeof stack[0], "%s", name);
            if (is_prot) protect++;
            if (!raw && opts->normalize_indent) {
                for (int d = 0; d < depth; d++) {
                    if (opts->indent_spaces > 0)
                        for (int s = 0; s < opts->indent_spaces; s++) buf_addc(&b, ' ');
                    else buf_addc(&b, '\t');
                }
            }
            depth++;
        } else if (ec < 0) {
            int nd = depth > 0 ? depth - 1 : 0;
            if (!raw && opts->normalize_indent) {
                for (int d = 0; d < nd; d++) {
                    if (opts->indent_spaces > 0)
                        for (int s = 0; s < opts->indent_spaces; s++) buf_addc(&b, ' ');
                    else buf_addc(&b, '\t');
                }
            }
            depth = nd;
            if (stack_n > 0) stack_n--;
            if (stack_n > 0 && (in_list(stack[stack_n - 1], PROTECTED_ENVS) ||
                                in_list(stack[stack_n - 1], MATH_ENVS)) && protect > 0)
                protect--;
        } else if (!raw && opts->normalize_indent) {
            for (int d = 0; d < depth; d++) {
                if (opts->indent_spaces > 0)
                    for (int s = 0; s < opts->indent_spaces; s++) buf_addc(&b, ' ');
                else buf_addc(&b, '\t');
            }
        }

        /* Emit the line body. */
        size_t body_len = llen;
        const char *body = line;
        if (!raw) {
            size_t lw = lead_ws(line);
            body = line + lw; body_len = llen - lw;
            if (opts->trim_trailing_ws) body_len = trim_end(body, body_len);
        }
        buf_add(&b, body, body_len);

        /* Hard-wrap joining: if this line and the next are both soft text,
         * merge them with a single space on this line and skip the next. */
        if (!raw && opts->join_hard_wraps && li + 1 < nlines) {
            size_t nl = lens[li + 1];
            const char *nxt = src + offs[li + 1];
            memcpy(copy, nxt, nl); copy[nl] = 0;
            char *nt = copy;
            while (*nt == ' ' || *nt == '\t') nt++;
            char nname[64];
            int nec = env_command(nt, nname, sizeof nname);
            if (!nec && !is_comment_line(nt) && joinable(nxt, nl) &&
                body_len > 0 && joinable(line, llen)) {
                buf_addc(&b, ' ');
                size_t nlw = lead_ws(nxt), nb = nl - nlw;
                const char *nbp = nxt + nlw;
                if (opts->trim_trailing_ws) nb = trim_end(nbp, nb);
                buf_add(&b, nbp, nb);
                li++;  /* consumed the next line */
            }
        }
        buf_addc(&b, '\n');
    }
    if (!trailing_nl && b.len && b.p[b.len - 1] == '\n') b.len--;
    if (b.p) b.p[b.len] = 0;
    else { b.p = (char *)malloc(1); if (!b.p) { free(offs); free(lens); free(copy); return -1; } b.p[0] = 0; }

    *out = b.p;
    free(offs); free(lens); free(copy);
    return 0;
}
