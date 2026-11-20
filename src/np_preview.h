/* np_preview.h - support for the unified preview panel:
 *   - Markdown: rendered by CUI directly (no helper here)
 *   - LaTeX:    async xelatex/pdflatex -> PDF -> pdftoppm PNG
 *   - HTML:     built-in loopback HTTP server + native WebView window
 *
 * All tools are optional at runtime: the UI degrades gracefully with a clear
 * message when an engine, a PDF rasterizer or a browser is not installed.
 */
#ifndef NP_PREVIEW_H
#define NP_PREVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_PREVIEW_AUTO = 0,
    NP_PREVIEW_MARKDOWN,
    NP_PREVIEW_LATEX,
    NP_PREVIEW_PDF,
    NP_PREVIEW_HTML,
    NP_PREVIEW_OFF
} np_preview_kind;

#define NP_PV_PATH_CAP 1024

np_preview_kind np_preview_detect(const char *path);
const char *np_preview_kind_name(np_preview_kind k);

/* ---- Async LaTeX compile ------------------------------------------- */
/* state: 0 idle, 1 running, 2 ok, 3 error. Written by the worker thread;
 * read by the UI. `state` is set last so a finished job always sees its log. */
typedef struct {
    volatile int state;
    char tex_path[NP_PV_PATH_CAP];
    char work_dir[NP_PV_PATH_CAP];
    char pdf_path[NP_PV_PATH_CAP];
    char base[NP_PV_PATH_CAP];       /* page images: <base>-1.png, -2.png ... */
    char cache_dir[NP_PV_PATH_CAP];  /* <work_dir>/.latex_cache (compile artifacts) */
    bool cache_artifacts;            /* when true, .aux/.log/.synctex/.pdf go to cache_dir */
    char log[4096];
    int pages;                       /* number of rendered pages (0 until known) */
    float page_w, page_h;            /* PDF page size in points (for SyncTeX hits) */
    uint64_t content_seen;           /* content_rev the job was started for */
    void *opaque;                    /* thread handle, internal */
    void *synctex;                   /* np_synctex*, when a .synctex file exists */
} np_tex_job;

void np_tex_job_init(np_tex_job *job);
int  np_tex_compile(np_tex_job *job, const char *tex_path);  /* 1 = started */
int  np_tex_render_pdf(np_tex_job *job, const char *pdf_path); /* render an existing PDF */
int  np_tex_state(const np_tex_job *job);
void np_tex_job_free(np_tex_job *job);
/* Path of page index (1-based) into buf; returns 0 when out of range/unknown. */
int  np_tex_page_path(const np_tex_job *job, int page, char *buf, size_t cap);
/* SyncTeX hit test: page is 1-based, x/y are PDF points from the top-left. */
int  np_tex_locate_source(np_tex_job *job, int page, float x, float y,
                          char *file, size_t file_cap, int *line);

/* ---- Built-in loopback HTTP server --------------------------------- */
typedef struct {
    volatile int state;         /* 0 stopped, 1 running */
    int port;                   /* bound port once running */
    char root[NP_PV_PATH_CAP];  /* directory served */
    void *opaque;               /* internal */
    char click_text[512];       /* last text clicked inside the previewed page */
    volatile int click_pending; /* set by the HTTP thread, cleared by the UI */
} np_http_server;

void np_http_init(np_http_server *s);
int  np_http_start(np_http_server *s, const char *root_dir);  /* port or 0 */
void np_http_stop(np_http_server *s);
int  np_http_running(const np_http_server *s);
/* Consumes a click reported by the injected preview script. Returns 1 and
 * copies the clicked element text into buf when a new one is waiting. */
int  np_http_take_click(np_http_server *s, char *buf, size_t cap);

/* ---- Browser / WebView launch -------------------------------------- */
/* Opens url in a chromeless app window when a Chromium browser is found
 * (Edge/Chrome --app=), otherwise in the system default browser.
 * Returns 1 when something was launched. */
int  np_webview_open(const char *url);
void np_open_url(const char *url);

#ifdef __cplusplus
}
#endif
#endif
