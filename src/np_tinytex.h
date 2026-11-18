#ifndef NP_TINYTEX_H
#define NP_TINYTEX_H

/* TinyTeX detection + automatic deployment (desktop builds).
 *
 * The editor never requires a TeX distribution at build time. At run time:
 *   1. np_tinytex_engine() finds xelatex/pdflatex on PATH or in the app-private
 *      TinyTeX install (so normal TeX Live installs keep working untouched).
 *   2. If none is found, np_tinytex_install_start() downloads the tiny
 *      TinyTeX-0 base package from GitHub releases and installs it privately.
 *      Missing LaTeX packages are then pulled on demand by tlmgr during
 *      compilation (TinyTeX's standard behaviour).
 *
 * All heavy work runs on a caller-provided background thread: the functions
 * are synchronous and communicate progress/cancel through shared variables.
 * Android builds use inert stubs (no terminal to run a TeX engine). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile long *progress;   /* 0..100 */
    volatile bool *cancel;
    char *err;                 /* optional error buffer */
    size_t err_cap;
} np_tinytex_opts;

/* Return an absolute path to a usable engine ("xelatex" preferred), copying
 * into out. Returns true and writes a bare command name if it is on PATH, or
 * an absolute path if it is inside the private install. */
bool np_tinytex_engine(char *out, size_t cap);

/* True when an app-private TinyTeX install exists (with its token file). */
bool np_tinytex_installed(void);

/* Directory holding the private install (for UI display). */
void np_tinytex_dir(char *out, size_t cap);

/* Install status, for a progress UI. */
int  np_tinytex_state(void);        /* 0 idle, 1 installing, 2 done, 3 failed */
const char *np_tinytex_status_text(void);

/* Synchronous install. Call from a background thread; poll state/progress.
 * `packages` (optional, e.g. "ctex xecjk beamer") are tlmgr-installed after
 * the base install so the common Chinese/beam templates compile first try. */
bool np_tinytex_install(const char *packages, const np_tinytex_opts *opts);

/* Append the private install's bin directory to a PATH-style value string.
 * Used by the compile worker so xelatex/tlmgr are found. Returns the same buf. */
void np_tinytex_path_value(char *buf, size_t cap);

/* Reset a failed state back to idle. */
void np_tinytex_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NP_TINYTEX_H */
