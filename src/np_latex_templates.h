#ifndef NP_LATEX_TEMPLATES_H
#define NP_LATEX_TEMPLATES_H

/* Built-in, self-contained LaTeX templates. Each one is a complete single
 * .tex file that compiles on its own (verified with TeX Live 2024). The editor
 * copies the selected text into a new tab; no external cls/sty/image needed. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;         /* menu / dialog label */
    const char *desc;         /* one-line description */
    const char *filename;     /* suggested filename for Save As */
    const char *text;         /* full .tex source, NUL terminated */
} np_latex_template;

int np_latex_template_count(void);
const np_latex_template *np_latex_template_at(int index);

#ifdef __cplusplus
}
#endif

#endif /* NP_LATEX_TEMPLATES_H */
