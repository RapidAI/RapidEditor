#ifndef NP_TEMPLATES_ONLINE_H
#define NP_TEMPLATES_ONLINE_H

/* Online LaTeX template catalogue for the built-in template picker.
 *
 * Ships a curated list of well-known GitHub LaTeX templates (CVs, theses,
 * papers) that compile as multi-file projects. Also parses GitHub's repository
 * search API so users can find more. Downloading a template yields a GitHub
 * codeload ZIP, which the app feeds straight into the ZIP importer. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[96];         /* display name */
    char full_name[128];   /* "owner/repo" */
    char desc[256];        /* one-line description */
    char branch[64];       /* default branch (master/main) */
    int  stars;            /* GitHub stars, -1 if unknown */
} np_online_template;

#define NP_ONT_MAX 32

/* Curated templates always available (no network). Returns count. */
int  np_online_curated(np_online_template *out, int cap);

/* Search GitHub for LaTeX templates. Synchronous — call on a worker thread.
 * Fills out with up to cap entries; returns count, or -1 on error (err filled).
 * `progress`/`cancel` can be NULL. */
int  np_online_search(const char *query, np_online_template *out, int cap,
                      volatile long *progress, volatile bool *cancel,
                      char *err, size_t err_cap);

/* Build the codeload ZIP URL for a template into buf. */
void np_online_zip_url(const np_online_template *t, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NP_TEMPLATES_ONLINE_H */
