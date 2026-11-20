#ifndef NP_LATEX_TABLE_H
#define NP_LATEX_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

/* Result of parsing a pasted CSV/TSV clipboard grid into a rectangular table.
 * Cells are UTF-8, NUL-terminated, stored row-major (rows * cols). Every cell
 * is independently allocated; free the whole thing with np_latex_table_free. */
typedef struct {
    char **cells;      /* rows*cols pointers, or NULL when empty */
    int rows;
    int cols;
} np_latex_table;

/* Parsing options -------------------------------------------------------- */
typedef enum {
    NP_TBL_DELIM_AUTO = 0,   /* sniff: tab > semicolon > comma > pipe */
    NP_TBL_DELIM_TAB,
    NP_TBL_DELIM_COMMA,
    NP_TBL_DELIM_SEMI,
    NP_TBL_DELIM_PIPE
} np_table_delim;

typedef struct {
    np_table_delim delim;    /* NP_TBL_DELIM_AUTO by default (pass 0) */
    bool first_row_header;   /* render first row with \toprule/\hline header */
    bool booktabs;           /* use booktabs (\toprule/\midrule/\bottomrule) */
    bool longtable;          /* emit longtable env for multipage tables */
    int  max_cols;           /* 0 = no limit; else truncate/warn */
} np_table_parse_opts;

/* Parse `text` into a table. Returns 0 on success, -1 on invalid/empty input.
 * Handles quoted fields ("a,b"), doubled quotes ("a""b"), CRLF, trailing
 * newline, ragged rows (padded with empty cells) and a leading BOM. */
int  np_latex_table_parse(const char *text, const np_table_parse_opts *opts,
                          np_latex_table *out, char *err, size_t err_cap);
void np_latex_table_free(np_latex_table *t);

/* Rendering options ------------------------------------------------------ */
typedef struct {
    bool booktabs;          /* 1 = booktabs rules, 0 = \hline grid */
    bool first_row_header;  /* emphasize first row */
    bool longtable;         /* longtable environment instead of table+tabular */
    const char *align;      /* e.g. "lcr"; NULL/"" -> all 'l'. Length clamped to cols */
    const char *caption;    /* optional, may be empty */
    const char *label;      /* optional, may be empty (e.g. "tab:foo") */
    float width;            /* scale box width 0..1; 0 = no resizebox */
} np_table_render_opts;

/* Render a parsed table to LaTeX source into `out`. Returns bytes written
 * (excluding NUL) or -1 if out is too small. Always NUL-terminates. */
int  np_latex_table_render(const np_latex_table *t, const np_table_render_opts *opts,
                           char *out, size_t cap);

/* Escape a cell's special LaTeX characters (& % $ # _ { } ~ ^ \ ).
 * Writes into out, NUL-terminates, returns bytes written. */
size_t np_latex_escape_cell(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
