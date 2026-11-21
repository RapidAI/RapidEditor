/* np_latex_html.c - lightweight LaTeX -> HTML converter for the preview pane.
 *
 * Scope: the common constructs of article/report-style documents:
 *   - preamble: \title / \author / \date captured, \documentclass and
 *     \usepackage dropped, body after \begin{document} rendered (snippet
 *     mode when there is no \begin{document})
 *   - sectioning: \section / \subsection / \subsubsection (approximate
 *     numbering), \paragraph, \maketitle, abstract
 *   - lists: itemize / enumerate / description (incl. \item[x]), nested
 *   - verbatim / lstlisting / minted byte-for-byte
 *   - math: $..$ / $$..$$ / \(..\) / \[..\] and equation / align / gather /
 *     displaymath / math / multline / eqnarray (+ starred) via KaTeX
 *   - tabular / tabular* / tabularx / longtable -> HTML table (basic,
 *     incl. \multicolumn, rules skipped)
 *   - inline: textbf/textit/emph/texttt/textsc/textrm/textsf/underline,
 *     \ref / \eqref / \label / \cite / href / verb, footnotes,
 *     smart quotes, -- / --- dashes, % comments
 * Unknown commands are dropped but their braced argument text survives, so
 * unfamiliar macros degrade to their content instead of vanishing.
 *
 * Output references KaTeX at /katex/... (bundled in the APK and served by the
 * loopback preview server); if the local assets are missing the page falls
 * back to the jsDelivr CDN.
 */
#include "np_latex_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Growable string buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct sb {
    char *p;
    size_t n, cap;
} sb;

static int sb_reserve(sb *b, size_t need) {
    if (b->n + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need + 1) cap *= 2;
    char *np = (char *)realloc(b->p, cap);
    if (!np) return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static void sb_putc(sb *b, char c) {
    if (sb_reserve(b, 1)) b->p[b->n++] = c;
}

static void sb_putn(sb *b, const char *s, size_t n) {
    if (s && n && sb_reserve(b, n)) {
        memcpy(b->p + b->n, s, n);
        b->n += n;
    }
}

static void sb_puts(sb *b, const char *s) { sb_putn(b, s, strlen(s)); }

/* ------------------------------------------------------------------ */
/* Escaping helpers                                                    */
/* ------------------------------------------------------------------ */

static void sb_esc_html(sb *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_puts(b, "&amp;");
        else if (c == '<') sb_puts(b, "&lt;");
        else if (c == '>') sb_puts(b, "&gt;");
        else sb_putc(b, c);
    }
}

/* Escape for a data-tex attribute: KaTeX receives the attribute value
 * verbatim, so keep \ and $ intact and only neutralize markup/quotes. */
static void sb_esc_attr(sb *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_puts(b, "&amp;");
        else if (c == '<') sb_puts(b, "&lt;");
        else if (c == '>') sb_puts(b, "&gt;");
        else if (c == '"') sb_puts(b, "&quot;");
        else sb_putc(b, c);
    }
}

/* ------------------------------------------------------------------ */
/* Small scanning helpers                                              */
/* ------------------------------------------------------------------ */

static int lx_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static const void *lx_memfind(const char *h, size_t hn, const char *nd, size_t nn) {
    if (!nn || nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, nd, nn)) return h + i;
    return NULL;
}

/* Read a braced group starting at s[*i] == '{' (any nesting, escapes
 * honored). Returns a malloc'd copy of the content, or NULL; on success
 * *i points just past '}'. */
static char *lx_read_braced(const char *s, size_t n, size_t *i) {
    if (!s || *i >= n || s[*i] != '{') return NULL;
    int depth = 0;
    size_t start = ++*i;
    for (; *i < n; (*i)++) {
        if (s[*i] == '\\') { if (*i + 1 < n) (*i)++; continue; }
        if (s[*i] == '{') depth++;
        else if (s[*i] == '}') {
            if (--depth < 0) {
                size_t len = *i - start;
                char *out = (char *)malloc(len + 1);
                if (out) { memcpy(out, s + start, len); out[len] = 0; }
                (*i)++;
                return out;
            }
        }
    }
    return NULL;
}

/* Read a braced group after optional whitespace; NULL if next non-ws char
 * is not '{'. On hit *i advances past '}', on miss *i is untouched. */
static char *lx_opt_braced(const char *s, size_t n, size_t *i) {
    size_t j = *i;
    while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
    if (j < n && s[j] == '{') {
        *i = j;
        return lx_read_braced(s, n, i);
    }
    return NULL;
}

/* Read an optional [..] argument after optional whitespace. */
static char *lx_opt_bracket(const char *s, size_t n, size_t *i) {
    size_t j = *i;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j < n && s[j] == '[') {
        size_t start = ++j;
        while (j < n && s[j] != ']') { if (s[j] == '\\' && j + 1 < n) j++; j++; }
        if (j < n) {
            size_t len = j - start;
            char *out = (char *)malloc(len + 1);
            if (out) { memcpy(out, s + start, len); out[len] = 0; }
            *i = j + 1;
            return out;
        }
    }
    return NULL;
}

/* Offset of the matching \end{name} from `from`, or n when unterminated. */
static size_t lx_env_end(const char *s, size_t n, size_t from, const char *name) {
    char begin[96], end[96];
    snprintf(begin, sizeof begin, "\\begin{%s}", name);
    snprintf(end, sizeof end, "\\end{%s}", name);
    size_t bl = strlen(begin), el = strlen(end);
    int depth = 0;
    for (size_t i = from; i + el <= n; ) {
        if (!memcmp(s + i, begin, bl)) { depth++; i += bl; continue; }
        if (!memcmp(s + i, end, el)) {
            if (depth == 0) return i;
            depth--;
            i += el;
            continue;
        }
        i++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Label registry (for \ref display)                                   */
/* ------------------------------------------------------------------ */

typedef struct lx_labels {
    char **names;
    int count, cap;
} lx_labels;

static void lx_collect_labels(const char *s, size_t n, lx_labels *lb) {
    static const char key[] = "\\label{";
    size_t kl = sizeof key - 1;
    size_t off = 0;
    for (;;) {
        if (off >= n) break;
        const char *p = (const char *)lx_memfind(s + off, n - off, key, kl);
        if (!p) break;
        const char *e = strchr(p + kl, '}');
        if (!e || e > s + n) break;
        size_t len = (size_t)(e - (p + kl));
        if (lb->count == lb->cap) {
            lb->cap = lb->cap ? lb->cap * 2 : 16;
            char **nn = (char **)realloc(lb->names, (size_t)lb->cap * sizeof *nn);
            if (!nn) break;
            lb->names = nn;
        }
        char *name = (char *)malloc(len + 1);
        if (name) {
            memcpy(name, p + kl, len);
            name[len] = 0;
            lb->names[lb->count++] = name;
        }
        off = (size_t)(e - s) + 1;
    }
}

static int lx_label_num(const lx_labels *lb, const char *name) {
    if (!name) return 0;
    for (int i = 0; i < lb->count; i++)
        if (strcmp(lb->names[i], name) == 0) return i + 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Converter context                                                   */
/* ------------------------------------------------------------------ */

typedef struct lx_ctx {
    sb *o;                      /* output */
    const char *s;              /* full source (for block ranges) */
    size_t n;
    lx_labels labels;
    char *title, *author, *date;
    int para_open;              /* a <p> is currently open */
    int no_para;                /* inside heading/caption: suppress <p> */
    int sec[3];                 /* section counters */
    size_t steps;               /* safety budget against pathological input */
} lx_ctx;

#define LX_STEP_LIMIT (200u * 1000u * 1000u)

/* Returns 1 when the step budget is exhausted (callers bail out). */
static int lx_step(lx_ctx *c) {
    return ++c->steps > LX_STEP_LIMIT;
}

static void lx_para_open(lx_ctx *c) {
    if (!c->no_para && !c->para_open) { sb_puts(c->o, "<p>"); c->para_open = 1; }
}

/* conv_inline opens paragraphs only when scanning the source itself;
 * malloc'd argument strings (titles, terms, footnotes) render inline-only. */
static void lx_maybe_open(lx_ctx *c, int owned) {
    if (owned) lx_para_open(c);
}

static void lx_para_close(lx_ctx *c) {
    if (c->para_open) { sb_puts(c->o, "</p>\n"); c->para_open = 0; }
}

/* Emit a KaTeX element. display != 0 renders as a block formula. */
static void lx_math(lx_ctx *c, const char *tex, size_t len, int display) {
    /* Strip \label{...}: KaTeX does not support it. */
    static const char lab[] = "\\label{";
    sb body = { 0 };
    const char *p = tex;
    const char *endp = tex + len;
    while (p < endp) {
        const char *hit = (const char *)lx_memfind(p, (size_t)(endp - p), lab, sizeof lab - 1);
        if (!hit) { sb_putn(&body, p, (size_t)(endp - p)); break; }
        sb_putn(&body, p, (size_t)(hit - p));
        const char *e = strchr(hit + sizeof lab - 1, '}');
        if (!e || e > endp) break;
        p = e + 1;
    }
    const char *out = body.p ? body.p : tex;
    size_t outn = body.p ? body.n : len;
    while (outn > 0 && (out[0] == '\n' || out[0] == '\r' || out[0] == ' ' || out[0] == '\t')) { out++; outn--; }
    while (outn > 0 && (out[outn - 1] == '\n' || out[outn - 1] == '\r' ||
                        out[outn - 1] == ' ' || out[outn - 1] == '\t')) outn--;
    if (display) sb_puts(c->o, "<div class=\"np-math np-math-d\" data-tex=\"");
    else         sb_puts(c->o, "<span class=\"np-math\" data-tex=\"");
    sb_esc_attr(c->o, out, outn);
    sb_puts(c->o, display ? "\"></div>\n" : "\"></span>");
    free(body.p);
}

/* \today -> current date (locale-independent ISO form). */
static void lx_today(lx_ctx *c) {
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    snprintf(buf, sizeof buf, "%d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    sb_puts(c->o, buf);
}

/* ------------------------------------------------------------------ */
/* Inline conversion (scans its own string, not ctx->s)                */
/* ------------------------------------------------------------------ */

static void conv_inline(lx_ctx *c, const char *s, size_t len, int in_cell);

/* Wrap an argument in a tag. */
static void lx_tagged(lx_ctx *c, const char *arg, const char *tag,
                      const char *cls, int in_cell) {
    if (!arg) return;
    if (tag) {
        sb_puts(c->o, "<");
        sb_puts(c->o, tag);
        if (cls) { sb_puts(c->o, " class=\""); sb_puts(c->o, cls); sb_puts(c->o, "\""); }
        sb_puts(c->o, ">");
    }
    conv_inline(c, arg, strlen(arg), in_cell);
    if (tag) {
        sb_puts(c->o, "</");
        sb_puts(c->o, tag);
        sb_puts(c->o, ">");
    }
}

static void conv_inline(lx_ctx *c, const char *s, size_t len, int in_cell) {
    /* Scanning the source document (vs. a malloc'd argument copy)? */
    int owned = s >= c->s && s <= c->s + c->n;
    size_t i = 0;
    while (i < len) {
        if (lx_step(c)) return;
        char ch = s[i];

        /* Comments: % to end of line, unless escaped. */
        if (ch == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < len && s[i] != '\n') i++;
            continue;
        }

        /* Display math $$..$$ */
        if (ch == '$' && i + 1 < len && s[i + 1] == '$') {
            size_t start = i + 2;
            size_t j = start;
            while (j + 1 < len && !(s[j] == '$' && s[j + 1] == '$' && s[j - 1] != '\\')) j++;
            if (j + 1 >= len) j = len;                     /* unterminated */
            lx_maybe_open(c, owned);
            lx_math(c, s + start, (j > start ? j : start) - start, 1);
            i = (j + 2 <= len) ? j + 2 : len;
            continue;
        }
        /* Inline math $..$ */
        if (ch == '$') {
            size_t start = i + 1;
            size_t j = start;
            while (j < len && !(s[j] == '$' && s[j - 1] != '\\')) j++;
            lx_maybe_open(c, owned);
            lx_math(c, s + start, (j > start ? j : start) - start, 0);
            i = (j < len) ? j + 1 : len;
            continue;
        }
        /* \(..\) inline, \[..\] display */
        if (ch == '\\' && i + 1 < len && (s[i + 1] == '(' || s[i + 1] == '[')) {
            char close = s[i + 1] == '(' ? ')' : ']';
            size_t start = i + 2;
            size_t j = start;
            while (j + 1 < len && !(s[j] == '\\' && s[j + 1] == close)) j++;
            lx_maybe_open(c, owned);
            lx_math(c, s + start, j - start, close == ']');
            i = (j + 2 <= len) ? j + 2 : len;
            continue;
        }

        if (ch == '\\') {
            size_t j = i + 1;
            if (j >= len) { i++; continue; }
            if (!lx_alpha(s[j])) {
                if (s[j] == '\\') {                          /* \\ line break */
                    lx_maybe_open(c, owned);
                    sb_puts(c->o, "<br>");
                    i += 2;
                    continue;
                }
                lx_maybe_open(c, owned);
                sb_esc_html(c->o, s + j, 1);                 /* escaped punctuation */
                i = j + 1;
                continue;
            }
            size_t cs = j;
            while (j < len && lx_alpha(s[j])) j++;
            size_t cl = j - cs;
            const char *cmd = s + cs;

            /* Font/format commands with one braced argument. */
            struct { const char *name; size_t len; const char *tag; const char *cls; } tags[] = {
                { "textbf",   6, "b",    NULL    },
                { "textit",   6, "i",    NULL    },
                { "emph",     4, "em",   NULL    },
                { "texttt",   6, "code", NULL    },
                { "textsc",   6, "span", "np-sc" },
                { "textrm",   6, "span", "np-rm" },
                { "textsf",   6, "span", "np-sf" },
                { "text",     4, "span", "np-rm" },
                { "underline",9, "u",    NULL    },
                { "uline",    5, "u",    NULL    },
            };
            int handled = 0;
            for (size_t t = 0; t < sizeof tags / sizeof *tags; t++) {
                if (cl == tags[t].len && !memcmp(cmd, tags[t].name, cl)) {
                    char *a = lx_opt_braced(s, len, &j);
                    lx_maybe_open(c, owned);
                    lx_tagged(c, a, tags[t].tag, tags[t].cls, in_cell);
                    free(a);
                    i = j;
                    handled = 1;
                    break;
                }
            }
            if (handled) continue;

            /* Known brand macros. */
            if (cl == 5 && !memcmp(cmd, "LaTeX", 5)) {
                lx_maybe_open(c, owned); sb_puts(c->o, "LaTeX"); i = j; continue;
            }
            if (cl == 3 && !memcmp(cmd, "TeX", 3)) {
                lx_maybe_open(c, owned); sb_puts(c->o, "TeX"); i = j; continue;
            }
            if (cl == 3 && !memcmp(cmd, "and", 3)) {         /* \and in \author */
                lx_maybe_open(c, owned); sb_puts(c->o, " &middot; "); i = j; continue;
            }
            if (cl == 5 && !memcmp(cmd, "today", 5)) {
                lx_maybe_open(c, owned); lx_today(c); i = j; continue;
            }

            if (cl == 8 && !memcmp(cmd, "footnote", 8)) {
                char *a = lx_opt_braced(s, len, &j);
                lx_maybe_open(c, owned);
                sb_puts(c->o, "<sup class=\"np-fn\">(");
                if (a) conv_inline(c, a, strlen(a), 0);
                sb_puts(c->o, ")</sup>");
                free(a);
                i = j;
                continue;
            }
            if (cl == 5 && !memcmp(cmd, "label", 5)) {
                char *a = lx_opt_braced(s, len, &j);
                if (a) {
                    sb_puts(c->o, "<a id=\"lbl-");
                    sb_esc_html(c->o, a, strlen(a));
                    sb_puts(c->o, "\"></a>");
                }
                free(a);
                i = j;
                continue;
            }
            if ((cl == 3 && !memcmp(cmd, "ref", 3)) ||
                (cl == 5 && !memcmp(cmd, "eqref", 5)) ||
                (cl == 7 && !memcmp(cmd, "autoref", 7)) ||
                (cl == 4 && !memcmp(cmd, "cref", 4))) {
                char *a = lx_opt_braced(s, len, &j);
                lx_maybe_open(c, owned);
                int eq = cl == 5 && !memcmp(cmd, "eqref", 5);
                sb_puts(c->o, "<a class=\"np-ref\" href=\"#lbl-");
                if (a) sb_esc_html(c->o, a, strlen(a));
                sb_puts(c->o, eq ? "\">(" : "\">[");
                int num = lx_label_num(&c->labels, a);
                if (num > 0) { char nb[16]; snprintf(nb, sizeof nb, "%d", num); sb_puts(c->o, nb); }
                else if (a) sb_esc_html(c->o, a, strlen(a));
                sb_puts(c->o, eq ? ")</a>" : "]</a>");
                free(a);
                i = j;
                continue;
            }
            if (cl == 4 && !memcmp(cmd, "cite", 4)) {
                char *a = lx_opt_braced(s, len, &j);
                lx_maybe_open(c, owned);
                sb_puts(c->o, "<span class=\"np-cite\">[");
                if (a) sb_esc_html(c->o, a, strlen(a));
                sb_puts(c->o, "]</span>");
                free(a);
                i = j;
                continue;
            }
            if (cl == 4 && !memcmp(cmd, "href", 4)) {
                char *url = lx_opt_braced(s, len, &j);
                char *txt = lx_opt_braced(s, len, &j);
                lx_maybe_open(c, owned);
                sb_puts(c->o, "<a href=\"");
                if (url) sb_esc_html(c->o, url, strlen(url));
                sb_puts(c->o, "\">");
                if (txt) conv_inline(c, txt, strlen(txt), in_cell);
                sb_puts(c->o, "</a>");
                free(url);
                free(txt);
                i = j;
                continue;
            }
            if (cl == 4 && !memcmp(cmd, "verb", 4) && j < len && !lx_alpha(s[j])) {
                char d = s[j];
                size_t k = j + 1;
                while (k < len && s[k] != d) k++;
                lx_maybe_open(c, owned);
                sb_puts(c->o, "<code>");
                sb_esc_html(c->o, s + j + 1, k - j - 1);
                sb_puts(c->o, "</code>");
                i = (k < len) ? k + 1 : len;
                continue;
            }
            if (cl == 6 && !memcmp(cmd, "thanks", 6)) {
                size_t t = j;
                char *a = lx_opt_braced(s, len, &t);
                free(a);                                     /* dropped */
                i = t > j ? t : j;
                continue;
            }
            /* Spacing commands collapse to one space. */
            if ((cl == 4 && !memcmp(cmd, "quad", 4)) ||
                (cl == 5 && !memcmp(cmd, "qquad", 5)) ||
                (cl == 1 && (cmd[0] == ',' || cmd[0] == ';' || cmd[0] == ':' ||
                             cmd[0] == ' ' || cmd[0] == '!'))) {
                lx_maybe_open(c, owned);
                sb_puts(c->o, " ");
                i = j;
                continue;
            }

            /* Unknown command: keep the braced argument text if present,
             * otherwise just drop the command word. */
            size_t t = j;
            char *a = lx_opt_braced(s, len, &t);
            if (a) {
                lx_maybe_open(c, owned);
                conv_inline(c, a, strlen(a), in_cell);
                free(a);
                j = t;
            }
            i = j;
            continue;
        }

        if (ch == '~') { lx_maybe_open(c, owned); sb_puts(c->o, "&nbsp;"); i++; continue; }
        if (ch == '{' || ch == '}') { i++; continue; }       /* groups: transparent */
        if (ch == '&') { lx_maybe_open(c, owned); sb_puts(c->o, "&amp;"); i++; continue; }
        if (ch == '<') { lx_maybe_open(c, owned); sb_puts(c->o, "&lt;"); i++; continue; }
        if (ch == '>') { lx_maybe_open(c, owned); sb_puts(c->o, "&gt;"); i++; continue; }

        /* Smart quotes. */
        if (ch == '`' && i + 1 < len && s[i + 1] == '`') {
            lx_maybe_open(c, owned); sb_puts(c->o, "\xe2\x80\x9c"); i += 2; continue;   /* U+201C */
        }
        if (ch == '\'' && i + 1 < len && s[i + 1] == '\'') {
            lx_maybe_open(c, owned); sb_puts(c->o, "\xe2\x80\x9d"); i += 2; continue;   /* U+201D */
        }
        /* Dashes. */
        if (ch == '-' && i + 2 < len && s[i + 1] == '-' && s[i + 2] == '-') {
            lx_maybe_open(c, owned); sb_puts(c->o, "\xe2\x80\x94"); i += 3; continue;   /* U+2014 */
        }
        if (ch == '-' && i + 1 < len && s[i + 1] == '-') {
            lx_maybe_open(c, owned); sb_puts(c->o, "\xe2\x80\x93"); i += 2; continue;   /* U+2013 */
        }

        lx_maybe_open(c, owned);
        sb_esc_html(c->o, s + i, 1);
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* Tabular -> HTML table (basic)                                       */
/* ------------------------------------------------------------------ */

/* Skip a rule command (\hline \toprule \midrule \bottomrule \cline
 * \rowcolor \arrayrulecolor \specialrule) plus its optional braced args. */
static size_t lx_skip_rule(const char *s, size_t n, size_t i) {
    static const char *rules[] = { "\\toprule", "\\midrule", "\\bottomrule",
        "\\hline", "\\cline", "\\rowcolor", "\\arrayrulecolor", "\\specialrule",
        "\\noalign", "\\cmidrule" };
    for (;;) {
        int hit = 0;
        for (size_t k = 0; k < sizeof rules / sizeof *rules; k++) {
            size_t rl = strlen(rules[k]);
            if (i + rl <= n && !memcmp(s + i, rules[k], rl)) {
                i += rl;
                for (int arg = 0; arg < 4; arg++) {          /* up to 4 args */
                    size_t t = i;
                    char *a = lx_opt_braced(s, n, &t);
                    if (!a) break;
                    free(a);
                    i = t;
                }
                hit = 1;
                break;
            }
        }
        if (!hit) break;
        /* allow repeated rules / whitespace between them */
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }
    return i;
}

static void conv_tabular(lx_ctx *c, const char *spec, size_t from, size_t to) {
    const char *s = c->s;
    sb_puts(c->o, "<table class=\"np-table\"><tbody>\n");
    size_t i = from;
    while (i < to) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') { i++; continue; }
        if (s[i] == '\\') {
            size_t t = lx_skip_rule(s, to, i);
            if (t > i) { i = t; continue; }
            if (i + 1 < to && s[i + 1] == '\\') { i += 2; continue; }  /* stray \\ */
        }
        /* One row: until top-level \\ or region end. */
        sb_puts(c->o, "<tr>");
        int depth = 0;
        size_t j = i;
        while (j < to) {
            if (s[j] == '\\' && j + 1 < to) {
                if (s[j + 1] == '\\' && depth == 0) break;
                j += 2;
                continue;
            }
            if (s[j] == '{') depth++;
            else if (s[j] == '}') depth--;
            j++;
        }
        /* Cells: split on & at brace depth 0 within [i, j). */
        size_t cs = i;
        depth = 0;
        for (size_t k = i; k <= j; k++) {
            if (k < j) {
                if (s[k] == '\\') { k++; continue; }
                if (s[k] == '{') { depth++; continue; }
                if (s[k] == '}') { depth--; continue; }
                if (s[k] != '&' || depth != 0) continue;
            }
            size_t ce = k;
            /* \multicolumn{n}{align}{content} as the whole cell? */
            size_t t = cs;
            while (t < ce && (s[t] == ' ' || s[t] == '\t')) t++;
            char *nsp = NULL, *align = NULL, *content = NULL;
            if (t + 12 <= ce && !memcmp(s + t, "\\multicolumn", 12)) {
                size_t p = t + 12;
                nsp = lx_opt_braced(s, ce, &p);
                size_t p2 = p;
                align = lx_opt_braced(s, ce, &p2);
                size_t p3 = p2;
                content = lx_opt_braced(s, ce, &p3);
            }
            if (nsp && content) {
                sb_puts(c->o, "<td colspan=\"");
                sb_puts(c->o, nsp);
                sb_puts(c->o, "\">");
                conv_inline(c, content, strlen(content), 1);
            } else {
                sb_puts(c->o, "<td>");
                conv_inline(c, s + cs, ce - cs, 1);
            }
            sb_puts(c->o, "</td>");
            free(nsp);
            free(align);
            free(content);
            cs = k + 1;
        }
        sb_puts(c->o, "</tr>\n");
        i = (j + 2 <= to) ? j + 2 : to;
    }
    sb_puts(c->o, "</tbody></table>\n");
    (void)spec;
}

/* ------------------------------------------------------------------ */
/* Lists                                                               */
/* ------------------------------------------------------------------ */

/* Next top-level \item offset in [from, to), skipping comments, escaped
 * chars, and whole nested environments. Returns `to` when none. */
static size_t lx_next_item(const char *s, size_t from, size_t to) {
    size_t i = from;
    while (i < to) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < to && s[i] != '\n') i++;
            continue;
        }
        if (s[i] == '\\') {
            if (i + 5 <= to && !memcmp(s + i, "\\item", 5) &&
                (i + 5 >= to || !lx_alpha(s[i + 5])))
                return i;
            if (i + 7 <= to && !memcmp(s + i, "\\begin{", 7)) {
                size_t nm = i + 7;
                while (nm < to && lx_alpha(s[nm])) nm++;
                if (nm < to && s[nm] == '}') {
                    char env[64];
                    size_t el = nm - (i + 7);
                    if (el < sizeof env - 1) {
                        memcpy(env, s + i + 7, el);
                        env[el] = 0;
                        size_t e = lx_env_end(s, to, nm + 1, env);
                        if (e < to) {
                            char endtok[80];
                            snprintf(endtok, sizeof endtok, "\\end{%s}", env);
                            i = e + strlen(endtok);
                            continue;
                        }
                    }
                }
            }
            i += 2;                                          /* skip escape pair */
            continue;
        }
        i++;
    }
    return to;
}

static void conv_block(lx_ctx *c, size_t from, size_t to);

static void conv_list(lx_ctx *c, size_t from, size_t to, const char *env) {
    int is_desc = strcmp(env, "description") == 0;
    int is_enum = strcmp(env, "enumerate") == 0;
    sb_puts(c->o, is_desc ? "<dl>\n" : is_enum ? "<ol>\n" : "<ul>\n");
    size_t pos = lx_next_item(c->s, from, to);
    while (pos < to) {
        size_t nxt = lx_next_item(c->s, pos + 5, to);
        size_t h = pos + 5;
        char *term = lx_opt_bracket(c->s, nxt, &h);
        if (is_desc) {
            if (term) {
                sb_puts(c->o, "<dt>");
                conv_inline(c, term, strlen(term), 0);
                sb_puts(c->o, "</dt>\n<dd>");
            } else {
                sb_puts(c->o, "<dd>");
            }
        } else {
            sb_puts(c->o, "<li>");
            if (term) {
                sb_puts(c->o, "<span class=\"np-itemlabel\">[");
                conv_inline(c, term, strlen(term), 0);
                sb_puts(c->o, "]</span> ");
            }
        }
        c->no_para++;
        conv_block(c, h, nxt);
        c->no_para--;
        lx_para_close(c);
        sb_puts(c->o, is_desc ? "</dd>\n" : "</li>\n");
        free(term);
        pos = nxt;
    }
    sb_puts(c->o, is_desc ? "</dl>\n" : is_enum ? "</ol>\n" : "</ul>\n");
}

/* ------------------------------------------------------------------ */
/* Headings                                                            */
/* ------------------------------------------------------------------ */

static void lx_section(lx_ctx *c, int level, size_t *i) {
    char *a = lx_opt_braced(c->s, c->n, i);
    c->sec[level - 1]++;
    for (int k = level; k < 3; k++) c->sec[k] = 0;
    char num[48];
    if (level == 1) snprintf(num, sizeof num, "%d", c->sec[0]);
    else if (level == 2) snprintf(num, sizeof num, "%d.%d", c->sec[0], c->sec[1]);
    else snprintf(num, sizeof num, "%d.%d.%d", c->sec[0], c->sec[1], c->sec[2]);
    lx_para_close(c);
    sb_puts(c->o, level == 1 ? "<h2>" : level == 2 ? "<h3>" : "<h4>");
    sb_puts(c->o, num);
    sb_puts(c->o, "&nbsp;&nbsp;");
    c->no_para++;
    if (a) conv_inline(c, a, strlen(a), 0);
    c->no_para--;
    sb_puts(c->o, level == 1 ? "</h2>\n" : level == 2 ? "</h3>\n" : "</h4>\n");
    free(a);
}

/* ------------------------------------------------------------------ */
/* Verbatim capture                                                    */
/* ------------------------------------------------------------------ */

static void lx_verbatim(lx_ctx *c, size_t *i, size_t end, const char *env) {
    char endtok[64];
    snprintf(endtok, sizeof endtok, "\\end{%s}", env);
    size_t el = strlen(endtok);
    const char *s = c->s;
    size_t start = *i;
    size_t j = start;
    /* `end` is where \end{env} starts; the comparison must be able to read
     * the full token there, so scan up to end inclusive. */
    while (j <= end && j + el <= c->n && memcmp(s + j, endtok, el) != 0) j++;
    size_t len = j - start;
#ifdef LX_DEBUG
    fprintf(stderr, "lx_verbatim env=%s start=%zu j=%zu end=%zu len=%zu\n", env, start, j, end, len);
#endif
    if (len > 0 && s[start] == '\n') { start++; len--; }     /* after \begin{..} */
    if (len > 0 && s[start + len - 1] == '\n') len--;
    lx_para_close(c);
    sb_puts(c->o, "<pre>");
    sb_esc_html(c->o, s + start, len);
    sb_puts(c->o, "</pre>\n");
    *i = (j + el <= end + el && j < end) ? j + el : end;
}

/* ------------------------------------------------------------------ */
/* Environment classification                                         */
/* ------------------------------------------------------------------ */

static int lx_is_math_env(const char *env) {
    static const char *envs[] = { "equation", "equation*", "align", "align*",
        "alignat", "alignat*", "gather", "gather*", "displaymath", "math",
        "multline", "multline*", "eqnarray", "eqnarray*", "flalign", "flalign*" };
    for (size_t k = 0; k < sizeof envs / sizeof *envs; k++)
        if (strcmp(env, envs[k]) == 0) return 1;
    return 0;
}

static int lx_is_verbatim_env(const char *env) {
    return !strcmp(env, "verbatim") || !strcmp(env, "verbatim*") ||
           !strcmp(env, "lstlisting") || !strcmp(env, "minted");
}

static int lx_is_list_env(const char *env) {
    return !strcmp(env, "itemize") || !strcmp(env, "enumerate") ||
           !strcmp(env, "description");
}

static int lx_is_table_env(const char *env) {
    return !strcmp(env, "tabular") || !strcmp(env, "tabular*") ||
           !strcmp(env, "longtable") || !strcmp(env, "tabularx") ||
           !strcmp(env, "longtabu") || !strcmp(env, "tabu");
}

/* ------------------------------------------------------------------ */
/* Block conversion (ranges into ctx->s)                               */
/* ------------------------------------------------------------------ */

static void conv_block(lx_ctx *c, size_t from, size_t to) {
    const char *s = c->s;
    size_t i = from;

    while (i < to) {
        if (lx_step(c)) return;
        /* Whitespace / paragraph breaks. */
        if (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t') {
            size_t j = i;
            int nl = 0;
            while (j < to && (s[j] == '\n' || s[j] == '\r' || s[j] == ' ' || s[j] == '\t')) {
                if (s[j] == '\n') nl++;
                j++;
            }
            if (nl >= 2) lx_para_close(c);
            else if (nl == 1 && c->para_open) sb_puts(c->o, " ");
            i = j;
            continue;
        }
        /* Comments. */
        if (s[i] == '%' && (i == from || s[i - 1] != '\\')) {
            while (i < to && s[i] != '\n') i++;
            continue;
        }

        if (s[i] != '\\') {
            /* Whole plain line: inline handles $math$, quotes, comments,
             * escapes and unknown commands. Splitting shorter would break
             * multi-byte tokens like ``..'' or $..$ pairs. */
            size_t j = i;
            while (j < to && s[j] != '\n' && s[j] != '\r') j++;
            conv_inline(c, s + i, j - i, 0);
            i = j;
            continue;
        }

        /* A command: dispatch block-level constructs. */
        size_t j = i + 1;
        if (j >= to) break;
        if (!lx_alpha(s[j])) {
            conv_inline(c, s + i, 2, 0);                     /* escape pair / \\ */
            i += 2;
            continue;
        }
        size_t cs = j;
        while (j < to && lx_alpha(s[j])) j++;
        size_t cl = j - cs;
        const char *cmd = s + cs;

        /* \begin{env} ... \end{env} */
        if (cl == 5 && !memcmp(cmd, "begin", 5)) {
            char *env = lx_opt_braced(s, to, &j);
            if (!env) { i = j; continue; }
            size_t body = j;
            size_t epos = lx_env_end(s, to, body, env);
            char endtok[96];
            snprintf(endtok, sizeof endtok, "\\end{%s}", env);
            size_t after = (epos + strlen(endtok) <= to) ? epos + strlen(endtok) : to;

            lx_para_close(c);
            if (lx_is_verbatim_env(env)) {
                lx_verbatim(c, &body, epos, env);
            } else if (lx_is_list_env(env)) {
                conv_list(c, body, epos, env);
            } else if (lx_is_math_env(env)) {
                lx_math(c, s + body, epos - body, strcmp(env, "math") != 0);
            } else if (!strcmp(env, "abstract")) {
                sb_puts(c->o, "<div class=\"np-abstract\"><b>摘要</b>&emsp;");
                conv_block(c, body, epos);
                lx_para_close(c);
                sb_puts(c->o, "</div>\n");
            } else if (lx_is_table_env(env)) {
                size_t b2 = body;
                char *spec = lx_opt_bracket(s, epos, &b2);
                if (!spec) {
                    /* tabularx puts the spec after the width argument */
                    char *w = lx_opt_braced(s, epos, &b2);
                    free(w);
                    spec = lx_opt_bracket(s, epos, &b2);
                }
                conv_tabular(c, spec ? spec : "lll", b2, epos);
                free(spec);
            } else if (!strcmp(env, "table") || !strcmp(env, "figure") ||
                       !strcmp(env, "table*") || !strcmp(env, "figure*")) {
                conv_block(c, body, epos);
            } else if (!strcmp(env, "center")) {
                sb_puts(c->o, "<div class=\"np-center\">\n");
                conv_block(c, body, epos);
                sb_puts(c->o, "</div>\n");
            } else if (!strcmp(env, "quote") || !strcmp(env, "quotation")) {
                sb_puts(c->o, "<blockquote>\n");
                conv_block(c, body, epos);
                lx_para_close(c);
                sb_puts(c->o, "</blockquote>\n");
            } else if (!strcmp(env, "document")) {
                conv_block(c, body, epos);
            } else {
                /* Unknown environment: keep the content. */
                conv_block(c, body, epos);
            }
            i = after;
            free(env);
            continue;
        }

        /* \caption{...} inside floats. */
        if (cl == 7 && !memcmp(cmd, "caption", 7)) {
            char *a = lx_opt_braced(s, to, &j);
            lx_para_close(c);
            sb_puts(c->o, "<div class=\"np-caption\">");
            c->no_para++;
            if (a) conv_inline(c, a, strlen(a), 0);
            c->no_para--;
            sb_puts(c->o, "</div>\n");
            free(a);
            i = j;
            continue;
        }

        /* Sectioning. */
        if (cl == 7 && !memcmp(cmd, "section", 7)) {
            size_t t = j;
            char *short_a = lx_opt_bracket(s, to, &t);
            free(short_a);
            lx_section(c, 1, &t);
            i = t;
            continue;
        }
        if (cl == 10 && !memcmp(cmd, "subsection", 10)) {
            size_t t = j;
            char *short_a = lx_opt_bracket(s, to, &t);
            free(short_a);
            lx_section(c, 2, &t);
            i = t;
            continue;
        }
        if (cl == 13 && !memcmp(cmd, "subsubsection", 13)) {
            size_t t = j;
            char *short_a = lx_opt_bracket(s, to, &t);
            free(short_a);
            lx_section(c, 3, &t);
            i = t;
            continue;
        }
        if (cl == 9 && !memcmp(cmd, "paragraph", 9)) {
            char *a = lx_opt_braced(s, to, &j);
            lx_para_close(c);
            sb_puts(c->o, "<h5>");
            c->no_para++;
            if (a) conv_inline(c, a, strlen(a), 0);
            c->no_para--;
            sb_puts(c->o, "</h5>\n");
            free(a);
            i = j;
            continue;
        }
        if (cl == 9 && !memcmp(cmd, "maketitle", 9)) {
            lx_para_close(c);
            sb_puts(c->o, "<div class=\"np-titleblock\">\n");
            c->no_para++;
            if (c->title) {
                sb_puts(c->o, "<h1>");
                conv_inline(c, c->title, strlen(c->title), 0);
                sb_puts(c->o, "</h1>\n");
            }
            if (c->author) {
                sb_puts(c->o, "<div class=\"np-author\">");
                conv_inline(c, c->author, strlen(c->author), 0);
                sb_puts(c->o, "</div>\n");
            }
            if (c->date) {
                sb_puts(c->o, "<div class=\"np-date\">");
                conv_inline(c, c->date, strlen(c->date), 0);
                sb_puts(c->o, "</div>\n");
            }
            c->no_para--;
            sb_puts(c->o, "</div>\n");
            i = j;
            continue;
        }
        if (cl == 3 && !memcmp(cmd, "par", 3)) {
            lx_para_close(c);
            i = j;
            continue;
        }

        /* Silent skips: page breaks and spacing (name + optional * + args). */
        if ((cl == 7 && !memcmp(cmd, "newpage", 7)) ||
            (cl == 8 && !memcmp(cmd, "clearpage", 8)) ||
            (cl == 15 && !memcmp(cmd, "cleardoublepage", 15)) ||
            (cl == 6 && (!memcmp(cmd, "vspace", 6) || !memcmp(cmd, "hspace", 6)))) {
            size_t t = j;
            if (t < to && s[t] == '*') t++;
            for (int arg = 0; arg < 2; arg++) {
                char *a = lx_opt_braced(s, to, &t);
                if (!a) break;
                free(a);
            }
            i = t;
            continue;
        }
        if (cl == 15 && !memcmp(cmd, "tableofcontents", 15)) {
            lx_para_close(c);
            sb_puts(c->o, "<div class=\"np-toc\">（轻量预览不生成目录）</div>\n");
            i = j;
            continue;
        }
        if (cl == 15 && !memcmp(cmd, "includegraphics", 15)) {
            size_t t = j;
            char *opts = lx_opt_bracket(s, to, &t);
            free(opts);
            char *a = lx_opt_braced(s, to, &t);
            lx_para_close(c);
            sb_puts(c->o, "<div class=\"np-img\">🖼️ ");
            if (a) sb_esc_html(c->o, a, strlen(a));
            sb_puts(c->o, "（轻量预览不加载图片）</div>\n");
            free(a);
            i = t;
            continue;
        }
        if ((cl == 5 && !memcmp(cmd, "input", 5)) ||
            (cl == 7 && !memcmp(cmd, "include", 7))) {
            size_t t = j;
            char *a = lx_opt_braced(s, to, &t);
            free(a);
            i = t;
            continue;
        }
        if (cl == 12 && !memcmp(cmd, "bibliography", 12)) {
            size_t t = j;
            char *a = lx_opt_braced(s, to, &t);
            free(a);
            i = t;
            continue;
        }
        if (cl == 17 && !memcmp(cmd, "printbibliography", 17)) {
            i = j;
            continue;
        }
        if (cl == 4 && !memcmp(cmd, "item", 4)) {
            /* Stray \item outside a list environment. */
            lx_para_close(c);
            sb_puts(c->o, "<ul><li>");
            conv_block(c, j, to);
            lx_para_close(c);
            sb_puts(c->o, "</li></ul>\n");
            return;
        }

        /* Anything else: inline fallback (drops the name, keeps the
         * braced argument). Extend the span over immediately following
         * braced groups so inline sees command + arguments as a unit. */
        size_t t = j;
        if (t < to && s[t] == '*') t++;
        for (int arg = 0; arg < 4; arg++) {
            char *a = lx_opt_braced(s, to, &t);
            if (!a) break;
            free(a);
        }
        conv_inline(c, s + i, t - i, 0);
        i = t;
    }
}

/* ------------------------------------------------------------------ */
/* Preamble                                                            */
/* ------------------------------------------------------------------ */

static char *lx_preamble_arg(const char *s, size_t n, const char *cmd) {
    char tok[64];
    snprintf(tok, sizeof tok, "\\%s", cmd);
    size_t kl = strlen(tok);
    const char *p = (const char *)lx_memfind(s, n, tok, kl);
    if (!p) return NULL;
    size_t i = (size_t)(p - s) + kl;
    return lx_opt_braced(s, n, &i);
}

/* ------------------------------------------------------------------ */
/* HTML template                                                       */
/* ------------------------------------------------------------------ */

static const char *NP_LATEX_HTML_HEAD =
"<!doctype html>\n"
"<html lang=\"zh\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>LaTeX 预览</title>\n"
"<link rel=\"stylesheet\" href=\"/katex/katex.min.css\"\n"
"      onerror=\"this.onerror=null;this.href='https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css'\">\n"
"<script defer src=\"/katex/katex.min.js\"\n"
"        onerror=\"var s=document.createElement('script');s.src='https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js';s.defer=true;document.head.appendChild(s);\"></script>\n"
"<style>\n"
"body{font-family:Georgia,'Times New Roman','Songti SC',SimSun,serif;margin:0 auto;"
"max-width:52rem;padding:1.2rem 1.4rem 4rem;line-height:1.65;color:#1c1c1c;background:#fff}\n"
"h1{font-size:1.7rem;margin:.4rem 0 .6rem;text-align:center}\n"
"h2{border-bottom:1px solid #ddd;padding-bottom:.2rem;margin-top:1.6rem}\n"
"h5{margin:.9rem 0 .2rem;font-style:italic}\n"
"pre{background:#f6f6f8;border:1px solid #e3e3e8;border-radius:6px;padding:.7rem .9rem;"
"overflow-x:auto;font-size:.85rem}\n"
"code{font-family:Consolas,Menlo,monospace;font-size:.92em;background:#f3f3f6;"
"padding:.06em .3em;border-radius:3px}\n"
"pre code{background:none;padding:0}\n"
".np-titleblock{margin:1rem 0 1.6rem;text-align:center}\n"
".np-author,.np-date{color:#555;font-size:.95rem;margin-top:.2rem}\n"
".np-abstract{margin:1rem 1.5rem;padding:.7rem 1rem;background:#f8f8fa;"
"border-left:3px solid #999;border-radius:4px}\n"
".np-table{border-collapse:collapse;margin:.8rem auto;font-size:.92rem;min-width:40%}\n"
".np-table td{border:1px solid #bbb;padding:.3rem .6rem}\n"
".np-table tbody{border-top:2px solid #333;border-bottom:2px solid #333}\n"
".np-caption{text-align:center;color:#444;font-size:.88rem;margin:.4rem 0}\n"
".np-center{text-align:center}\n"
".np-fn{color:#555;font-size:.72rem}\n"
".np-ref,.np-cite{color:#36c;text-decoration:none}\n"
".np-sc{font-variant:small-caps}\n"
".np-rm{font-family:inherit}\n"
".np-sf{font-family:Arial,'Helvetica Neue',sans-serif}\n"
".np-itemlabel{color:#555}\n"
".np-img{background:#f3f3f6;border:1px dashed #bbb;border-radius:6px;"
"padding:.6rem;text-align:center;color:#666;margin:.6rem 0}\n"
".np-toc{color:#888;font-style:italic;margin:.6rem 0}\n"
".np-math-d{display:block;text-align:center;margin:.7rem 0;overflow-x:auto}\n"
".np-math-err{color:#b00;font-family:monospace;font-size:.85rem}\n"
".np-lite-note{font-size:.75rem;color:#999;text-align:center;margin-bottom:1rem}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"np-lite-note\">轻量预览（KaTeX 渲染，非编译结果；结构与宏可能有差异）</div>\n";

static const char *NP_LATEX_HTML_TAIL =
"<script>\n"
"window.addEventListener('load',function(){\n"
"  function run(){\n"
"    if(!window.katex){return false;}\n"
"    var els=document.querySelectorAll('.np-math');\n"
"    for(var k=0;k<els.length;k++){\n"
"      var el=els[k],tex=el.getAttribute('data-tex');\n"
"      try{katex.render(tex,el,{displayMode:el.classList.contains('np-math-d')});}\n"
"      catch(e){el.textContent=tex;el.classList.add('np-math-err');}\n"
"    }\n"
"    return true;\n"
"  }\n"
"  if(!run()){\n"
"    var tries=0,iv=setInterval(function(){\n"
"      tries++;if(run()||tries>40)clearInterval(iv);\n"
"    },150);\n"
"  }\n"
"});\n"
"</script>\n"
"</body>\n</html>\n";

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

char *np_latex_html(const char *tex, size_t len, size_t *out_len) {
    if (!tex) { tex = ""; len = 0; }
    lx_ctx c;
    memset(&c, 0, sizeof c);
    c.s = tex;
    c.n = len;

    /* Preamble: capture title/author/date; locate the body. */
    size_t body_from = 0;
    {
        static const char bd[] = "\\begin{document}";
        const char *p = (const char *)lx_memfind(tex, len, bd, sizeof bd - 1);
        size_t pre_end = p ? (size_t)(p - tex) : len;
        body_from = p ? pre_end + sizeof bd - 1 : 0;
        c.title = lx_preamble_arg(tex, pre_end, "title");
        c.author = lx_preamble_arg(tex, pre_end, "author");
        c.date = lx_preamble_arg(tex, pre_end, "date");
    }

    lx_collect_labels(tex, len, &c.labels);

    sb out = { 0 };
    c.o = &out;
    sb_puts(&out, NP_LATEX_HTML_HEAD);
    conv_block(&c, body_from, len);
    lx_para_close(&c);
    sb_puts(&out, NP_LATEX_HTML_TAIL);

    for (int k = 0; k < c.labels.count; k++) free(c.labels.names[k]);
    free(c.labels.names);
    free(c.title);
    free(c.author);
    free(c.date);

    if (out.p) out.p[out.n] = 0;
    if (out_len) *out_len = out.n;
    return out.p;
}
