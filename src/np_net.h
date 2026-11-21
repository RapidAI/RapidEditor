#ifndef NP_NET_H
#define NP_NET_H

/* Small cross-platform HTTPS downloader for template ZIPs and TinyTeX
 * installers. Synchronous by design: callers run it on their own background
 * thread. Progress and cancellation are polled through shared variables so
 * the UI thread can update a progress bar / cancel at any time.
 *
 * Windows: WinHTTP (native, follows redirects, no extra DLLs).
 * POSIX:   libcurl if available at build time; otherwise the `curl` CLI. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile long *progress;   /* 0..100, written by the downloader */
    volatile bool *cancel;     /* set true by the UI to abort */
    char *err;                 /* optional buffer for a human-readable error */
    size_t err_cap;
    int timeout_sec;           /* 0 = default (120) */
} np_net_opts;

/* Download `url` to `dest`. Creates/truncates dest. Returns true on success
 * (HTTP 2xx and a non-empty file), false on any error (reason in opts->err). */
bool np_net_download(const char *url, const char *dest, const np_net_opts *opts);

/* Best-effort HTTP(S) JSON fetch into a heap buffer (NUL terminated). Caller
 * frees with free(). Returns NULL on error. Used for the GitHub search API. */
char *np_net_fetch(const char *url, const np_net_opts *opts);

/* POST a JSON body with an optional Bearer token. Returns the response body
 * in a heap buffer (caller frees), or NULL on any error / non-2xx status.
 * `err` receives a short reason. `status_out`, if non-NULL, receives the
 * HTTP status code (0 if no response). Used by the maclaw device-auth flow. */
char *np_net_post_json(const char *url, const char *bearer, const char *json_body,
                       const np_net_opts *opts, int *status_out);

/* GET with an optional Bearer token. Same response semantics as
 * np_net_post_json: heap buffer (caller frees) or NULL on error / non-2xx. */
char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *opts, int *status_out);

#ifdef __cplusplus
}
#endif

#endif /* NP_NET_H */
