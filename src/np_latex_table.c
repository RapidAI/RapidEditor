#include "np_latex_table.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define NP_TBL_MAX_ROWS 512
#define NP_TBL_MAX_COLS 64
#define NP_TBL_MAX_CELL 4096

/* Count a delimiter's occurrences outside double-quoted fields on the first
 * non-empty line. Used to sniff which separator the clipboard uses. */
static int snip_delim(const char *s, char d) {
    int n = 0;
    bool quoted = false;
    for (const char *p = s; *p; p++) {
        if (*p == '"') { quoted = !quoted; continue; }
        if (!quoted && *p == d) n++;
        if (!quoted && (*p == '\n' || *p == '\r')) break;
    }
    return n;
}

static char sniff_delim(const char *s, np_table_delim pref) {
    switch (pref) {
        case NP_TBL_DELIM_TAB:   return '\t';
        case NP_TBL_DELIM_COMMA: return ',';
        case NP_TBL_DELIM_SEMI:  return ';';
        case NP_TBL_DELIM_PIPE:  return '|';
        default: break;
    }
    int nt = snip_delim(s, '\t');
    int ns = snip_delim(s, ';');
    int nc = snip_delim(s, ',');
    int np = snip_delim(s, '|');
    int best = nt;
    char d = '\t';
    if (ns > best) { best = ns; d = ';'; }
    if (nc > best) { best = nc; d = ','; }
    if (np > best) { best = np; d = '|'; }
    if (best == 0) d = ','; /* single column: comma is harmless */
    return d;
}

/* Append one byte to a growing cell buffer. */
static void cell_push(char *buf, size_t *len, size_t cap, char c) {
    if (*len + 1 < cap) { buf[(*len)++] = c; buf[*len] = 0; }
}

int np_latex_table_parse(const char *text, const np_table_parse_opts *opts,
                         np_latex_table *out, char *err, size_t err_cap) {
    if (!out) return -1;
    out->cells = NULL; out->rows = 0; out->cols = 0;
    if (err && err_cap) err[0] = 0;
    if (!text || !*text) {
        if (err && err_cap) snprintf(err, err_cap, "粘贴内容为空");
        return -1;
    }
    np_table_parse_opts def;
    memset(&def, 0, sizeof def);
    def.delim = NP_TBL_DELIM_AUTO;
    if (!opts) opts = &def;

    /* Skip a UTF-8 BOM. */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) text += 3;

    char delim = sniff_delim(text, opts->delim);
    int max_cols = opts->max_cols > 0 ? opts->max_cols : NP_TBL_MAX_COLS;
    if (max_cols > NP_TBL_MAX_COLS) max_cols = NP_TBL_MAX_COLS;

    /* Growable row storage: each row is an array of cell char*. */
    char ***rows = (char ***)malloc(sizeof(char **) * NP_TBL_MAX_ROWS);
    int *rcols = (int *)malloc(sizeof(int) * NP_TBL_MAX_ROWS);
    if (!rows || !rcols) { free(rows); free(rcols); return -1; }
    int nrows = 0, max_seen = 0;

    const char *p = text;
    while (*p && nrows < NP_TBL_MAX_ROWS) {
        int ncol = 0;
        char **cur = NULL;
        /* One row. */
        for (;;) {
            if (ncol >= max_cols) break;
            char cell[NP_TBL_MAX_CELL];
            size_t clen = 0;
            cell[0] = 0;
            bool quoted = false;
            /* Leading spaces before a quote are insignificant. */
            const char *q = p;
            while (*q == ' ') q++;
            if (*q == '"') { quoted = true; p = q; }

            if (quoted) {
                p++; /* opening quote */
                while (*p) {
                    if (*p == '"') {
                        if (p[1] == '"') { cell_push(cell, &clen, sizeof cell, '"'); p += 2; continue; }
                        p++; /* closing quote */
                        break;
                    }
                    cell_push(cell, &clen, sizeof cell, *p++);
                }
                /* Skip spaces after closing quote up to the delimiter. */
                while (*p == ' ') p++;
            } else {
                while (*p && *p != delim && *p != '\n' && *p != '\r')
                    cell_push(cell, &clen, sizeof cell, *p++);
            }
            /* Trim trailing spaces for unquoted cells. */
            if (!quoted) {
                while (clen && cell[clen - 1] == ' ') cell[--clen] = 0;
            }
            char **tmp = (char **)realloc(cur, sizeof(char *) * (size_t)(ncol + 1));
            if (!tmp) {
                for (int i = 0; i < ncol; i++) free(cur[i]);
                free(cur);
                for (int i = 0; i < nrows; i++) { for (int j = 0; j < rcols[i]; j++) free(rows[i][j]); free(rows[i]); }
                free(rows); free(rcols);
                return -1;
            }
            cur = tmp;
            cur[ncol] = (char *)malloc(clen + 1);
            if (!cur[ncol]) {
                for (int i = 0; i < ncol; i++) free(cur[i]);
                free(cur);
                for (int i = 0; i < nrows; i++) { for (int j = 0; j < rcols[i]; j++) free(rows[i][j]); free(rows[i]); }
                free(rows); free(rcols);
                return -1;
            }
            memcpy(cur[ncol], cell, clen + 1);
            ncol++;

            if (*p == delim) { p++; continue; }
            break; /* end of row */
        }
        rows[nrows] = cur;
        rcols[nrows] = ncol;
        if (ncol > max_seen) max_seen = ncol;
        nrows++;
        /* Consume the line ending (\r\n, \n or \r). */
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        if (!*p) break;
    }

    int non_empty = 0;
    for (int i = 0; i < nrows && !non_empty; i++)
        for (int j = 0; j < rcols[i]; j++)
            if (rows[i][j][0]) { non_empty = 1; break; }
    if (nrows == 0 || max_seen == 0 || !non_empty) {
        if (err && err_cap) snprintf(err, err_cap, "没有解析到任何表格数据");
        for (int i = 0; i < nrows; i++) { for (int j = 0; j < rcols[i]; j++) free(rows[i][j]); free(rows[i]); }
        free(rows); free(rcols);
        return -1;
    }

    /* Allocate a rectangular grid, padding ragged rows with empty strings. */
    char **grid = (char **)calloc((size_t)nrows * (size_t)max_seen, sizeof(char *));
    if (!grid) {
        for (int i = 0; i < nrows; i++) { for (int j = 0; j < rcols[i]; j++) free(rows[i][j]); free(rows[i]); }
        free(rows); free(rcols);
        return -1;
    }
    for (int i = 0; i < nrows; i++) {
        for (int j = 0; j < max_seen; j++) {
            if (j < rcols[i]) grid[(size_t)i * max_seen + j] = rows[i][j];
            else grid[(size_t)i * max_seen + j] = (char *)calloc(1, 1);
        }
        free(rows[i]);
    }
    free(rows); free(rcols);

    out->cells = grid;
    out->rows = nrows;
    out->cols = max_seen;
    return 0;
}

void np_latex_table_free(np_latex_table *t) {
    if (!t || !t->cells) return;
    for (int i = 0; i < t->rows * t->cols; i++) free(t->cells[i]);
    free(t->cells);
    t->cells = NULL; t->rows = t->cols = 0;
}

size_t np_latex_escape_cell(const char *s, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t o = 0;
    out[0] = 0;
    if (!s) return 0;
    /* Emit a literal string but bound-check every write. */
#define EC_PUT1(c) do { if (o + 1 < cap) out[o++] = (c); } while (0)
#define EC_PUT(str) do { for (const char *_q = (str); *_q && o + 1 < cap; _q++) out[o++] = *_q; } while (0)
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&': case '%': case '$': case '#': case '_': case '{': case '}':
                EC_PUT1('\\'); EC_PUT1(*p); break;
            case '~': EC_PUT("\\textasciitilde{}"); break;
            case '^': EC_PUT("\\textasciicircum{}"); break;
            case '\\': EC_PUT("\\textbackslash{}"); break;
            default: EC_PUT1(*p); break;
        }
    }
#undef EC_PUT
#undef EC_PUT1
    out[o < cap ? o : cap - 1] = 0;
    return o;
}

#define APPEND(...) do { int _n = snprintf(out + o, cap - o, __VA_ARGS__); if (_n > 0 && (size_t)_n > cap - o - 1) return -1; o += (size_t)_n; } while (0)

int np_latex_table_render(const np_latex_table *t, const np_table_render_opts *opts,
                          char *out, size_t cap) {
    if (!t || !t->cells || !out || cap == 0) return -1;
    np_table_render_opts def;
    memset(&def, 0, sizeof def);
    if (!opts) opts = &def;
    int cols = t->cols, rows = t->rows;
    size_t o = 0;
    out[0] = 0;

    char align[NP_TBL_MAX_COLS + 8];
    size_t alen = 0;
    const char *asrc = (opts->align && *opts->align) ? opts->align : NULL;
    for (int c = 0; c < cols && alen < sizeof align - 2; c++) {
        char ch = asrc ? asrc[c % strlen(asrc)] : 'l';
        if (ch != 'l' && ch != 'c' && ch != 'r' && ch != 'p') ch = 'l';
        align[alen++] = ch;
    }
    align[alen] = 0;

    bool bt = opts->booktabs;
    bool hdr = opts->first_row_header && rows > 1;
    bool lt = opts->longtable;

    if (lt) {
        APPEND("\\begin{longtable}{%s}\n", align);
        if (opts->caption && *opts->caption) APPEND("\\caption{%s}\n", opts->caption);
        if (opts->label && *opts->label) APPEND("\\label{%s}\n", opts->label);
        APPEND("\\\\\n");
        APPEND(bt ? "\\toprule\n" : "\\hline\n");
    } else {
        APPEND("\\begin{table}[htbp]\n\\centering\n");
        if (opts->caption && *opts->caption) APPEND("\\caption{%s}\n", opts->caption);
        if (opts->label && *opts->label) APPEND("\\label{%s}\n", opts->label);
        if (opts->width > 0.01f)
            APPEND("\\resizebox{%.2f\\linewidth}{!}{\n", opts->width);
        APPEND("\\begin{tabular}{%s}\n", align);
        APPEND(bt ? "\\toprule\n" : "\\hline\n");
    }

    char esc[NP_TBL_MAX_CELL * 6];
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            np_latex_escape_cell(t->cells[(size_t)r * cols + c], esc, sizeof esc);
            APPEND("%s", esc);
            if (c + 1 < cols) APPEND(" & ");
        }
        APPEND(" \\\\\n");
        if (bt) {
            if (hdr && r == 0) APPEND("\\midrule\n");
            else if (!hdr && r == rows - 1) APPEND("\\bottomrule\n");
        } else {
            if (hdr && r == 0) APPEND("\\hline\n");
            else if (!hdr && r == rows - 1) APPEND("\\hline\n");
        }
    }
    /* With a header, booktabs still needs a closing bottomrule. */
    if (bt && hdr) APPEND("\\bottomrule\n");

    if (lt) {
        APPEND("\\end{longtable}\n");
    } else {
        APPEND("\\end{tabular}\n");
        if (opts->width > 0.01f) APPEND("}\n");
        APPEND("\\end{table}\n");
    }
    return (int)o;
}
