/* np_preview.c - cross-platform helpers for the unified preview panel.
 *
 * Kept in its own translation unit so the immediate-mode UI file stays focused.
 * Nothing here requires a tool to be installed at build time: every external
 * dependency (TeX engine, PDF rasterizer, browser) is resolved at run time and
 * a missing one is reported through the async job / server state.
 */
#include "np_preview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#elif defined(__ANDROID__)
#include <SDL.h> /* SDL_OpenURL */
/* Android (with the SDL2 backend) still uses POSIX sockets for the built-in
 * loopback HTTP server of the HTML preview. */
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#endif

/* Existence check: the Windows CRT calls it _access, POSIX calls it access. */
#ifdef _WIN32
#define np__access(p, m) _access((p), (m))
#else
#define np__access(p, m) access((p), (m))
#endif

/* ------------------------------------------------------------------ */
/* Preview kind detection                                              */
/* ------------------------------------------------------------------ */

static int pv_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const char *pv_base(const char *path) {
    const char *r = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') r = p + 1;
    return r;
}

np_preview_kind np_preview_detect(const char *path) {
    if (!path || !*path) return NP_PREVIEW_OFF;
    const char *base = pv_base(path);
    const char *dot = strrchr(base, '.');
    if (!dot) return NP_PREVIEW_OFF;
    dot++;
    if (pv_ieq(dot, "md") || pv_ieq(dot, "markdown") || pv_ieq(dot, "mdown") ||
        pv_ieq(dot, "mkd") || pv_ieq(dot, "mkdn") || pv_ieq(dot, "mdtxt"))
        return NP_PREVIEW_MARKDOWN;
    if (pv_ieq(dot, "tex") || pv_ieq(dot, "latex") || pv_ieq(dot, "ltx"))
        return NP_PREVIEW_LATEX;
    if (pv_ieq(dot, "pdf"))
        return NP_PREVIEW_PDF;
    if (pv_ieq(dot, "html") || pv_ieq(dot, "htm") || pv_ieq(dot, "xhtml") ||
        pv_ieq(dot, "svg"))
        return NP_PREVIEW_HTML;
    return NP_PREVIEW_OFF;
}

const char *np_preview_kind_name(np_preview_kind k) {
    switch (k) {
        case NP_PREVIEW_MARKDOWN: return "Markdown";
        case NP_PREVIEW_LATEX:    return "LaTeX";
        case NP_PREVIEW_PDF:      return "PDF";
        case NP_PREVIEW_HTML:     return "Web";
        default:                  return "关闭";
    }
}

/* ------------------------------------------------------------------ */
/* Small path helpers                                                  */
/* ------------------------------------------------------------------ */

static void pv_dir_of(const char *path, char *out, size_t cap) {
    const char *base = pv_base(path);
    size_t n = (size_t)(base - path);
    if (n == 0) { snprintf(out, cap, "."); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    /* strip trailing slash, but keep root like "C:\" or "/" */
    while (n > 1 && (out[n-1] == '/' || out[n-1] == '\\')) n--;
    out[n] = 0;
}

/* Directory that is safe to write compile artifacts into: the source file's
 * own directory, so relative \input{} and image includes keep working. */
static void pv_work_dir(const char *tex, char *out, size_t cap) {
    pv_dir_of(tex, out, cap);
}

static void pv_strip_ext(const char *path, char *out, size_t cap) {
    const char *base = pv_base(path);
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

/* Quote a path for the shell (double quotes, escape existing quotes). */
static void pv_quote(const char *s, char *out, size_t cap) {
    size_t j = 0;
    out[j++] = '"';
    for (const char *p = s; *p && j + 2 < cap; p++) {
        if (*p == '"') out[j++] = '\\';
        out[j++] = *p;
    }
    if (j + 1 < cap) out[j++] = '"';
    out[j] = 0;
}

/* Run a command line synchronously; returns the process exit code.
 * stdout/stderr are redirected by the caller's command string. */
static int pv_run(const char *cmd) {
#ifdef _WIN32
    return system(cmd);
#else
    return system(cmd);
#endif
}

/* Read the tail of a file into buf (used for the TeX log). */
static void pv_read_log_tail(const char *path, char *buf, size_t cap) {
    buf[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        long start = 0;
        if (sz > (long)cap - 1) start = sz - ((long)cap - 1);
        fseek(f, start, SEEK_SET);
        size_t n = fread(buf, 1, cap - 1, f);
        buf[n] = 0;
    }
    fclose(f);
}

/* Find an executable on PATH. Fills out with the token as typed (PATH lookup
 * is left to the shell), returns 1 when `command -v`/`where` reports it. */
static int pv_has_tool(const char *name) {
    char cmd[256];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "where %s >nul 2>nul", name);
#else
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
#endif
    return pv_run(cmd) == 0;
}

/* ------------------------------------------------------------------ */
/* Async LaTeX compile                                                  */
/* ------------------------------------------------------------------ */

typedef struct pv_tex_ctx {
    np_tex_job *job;
    int mode;                   /* 0 = compile .tex, 1 = render existing .pdf */
#ifdef _WIN32
    HANDLE th;
#else
    pthread_t th;
#endif
} pv_tex_ctx;

/* Render a PDF into numbered page images <base>-1.png ... <base>-N.png.
 * Returns the page count (>0) or 0 on failure. Writes a reason into j->log. */
static int pv_render_pdf(np_tex_job *j, const char *pdf, const char *base,
                         const char *qlog) {
    char cmd[NP_PV_PATH_CAP * 4];
    int pages = 0;
    char qpdf[NP_PV_PATH_CAP * 2], qbase[NP_PV_PATH_CAP * 2];
    pv_quote(pdf, qpdf, sizeof qpdf);
    pv_quote(base, qbase, sizeof qbase);

    if (pv_has_tool("pdftoppm")) {
        /* -singlefile would flatten to one image; instead emit prefix-1.png,
         * prefix-2.png ... by probing how many pages pdfinfo reports. */
        int n = 0;
        char info[256];
        snprintf(info, sizeof info, "pdfinfo %s", qpdf);
        /* Ask pdfinfo for the page count without a pipe: write to a temp file. */
        char info_path[NP_PV_PATH_CAP];
        char qinfo[NP_PV_PATH_CAP * 2];
        snprintf(info_path, sizeof info_path, "%s/rapideditor-pdfinfo.txt", j->work_dir);
        pv_quote(info_path, qinfo, sizeof qinfo);
        snprintf(cmd, sizeof cmd, "pdfinfo %s > %s 2>&1", qpdf, qinfo);
        if (pv_run(cmd) == 0) {
            FILE *f = fopen(info_path, "rb");
            if (f) {
                char line[512];
                while (fgets(line, sizeof line, f)) {
                    if (strncmp(line, "Pages:", 6) == 0) {
                        n = atoi(line + 6);
                        break;
                    }
                }
                fclose(f);
            }
        }
        remove(info_path);
        if (n < 1) n = 1;                        /* fall back to a single page */
        if (n > 200) n = 200;                    /* guard runaway documents */
        for (int p = 1; p <= n; p++) {
            snprintf(cmd, sizeof cmd,
                     "pdftoppm -f %d -l %d -r 110 -png -singlefile %s %s-%d >> %s 2>&1",
                     p, p, qpdf, qbase, p, qlog);
            pv_run(cmd);
            char one[NP_PV_PATH_CAP];
            snprintf(one, sizeof one, "%s-%d.png", base, p);
            if (np__access(one, 0) == 0) pages++;
            else break;
        }
    } else if (pv_has_tool("magick") || pv_has_tool("convert")) {
        const char *mc = pv_has_tool("magick") ? "magick" : "convert";
        /* ImageMagick reads all PDF pages into a %d sequence. */
        snprintf(cmd, sizeof cmd, "%s -density 110 %s %s-%%d.png >> %s 2>&1",
                 mc, qpdf, qbase, qlog);
        pv_run(cmd);
        for (int p = 1; p <= 200; p++) {
            char one[NP_PV_PATH_CAP];
            snprintf(one, sizeof one, "%s-%d.png", base, p);
            if (np__access(one, 0) == 0) pages++;
            else break;
        }
        if (pages == 0) {                        /* some builds start at 0 */
            char zero[NP_PV_PATH_CAP];
            snprintf(zero, sizeof zero, "%s-0.png", base);
            if (np__access(zero, 0) == 0) {
                char one[NP_PV_PATH_CAP];
                snprintf(one, sizeof one, "%s-1.png", base);
                rename(zero, one);
                pages = 1;
            }
        }
    } else {
        snprintf(j->log, sizeof j->log,
                 "已生成 PDF，但未找到 PDF 转图片工具。\n请安装 poppler (pdftoppm/pdfinfo) 或 ImageMagick。");
        return 0;
    }
    return pages;
}

static void pv_tex_worker(void *arg) {
    pv_tex_ctx *ctx = (pv_tex_ctx *)arg;
    np_tex_job *j = ctx->job;

    char log_path[NP_PV_PATH_CAP];
    char qdir[NP_PV_PATH_CAP * 2], qlog[NP_PV_PATH_CAP * 2];
    snprintf(log_path, sizeof log_path, "%s/rapideditor-tex.log", j->work_dir);
    pv_quote(j->work_dir, qdir, sizeof qdir);
    pv_quote(log_path, qlog, sizeof qlog);

    int pages = 0;
    char pdf[NP_PV_PATH_CAP];

    if (ctx->mode == 1) {
        /* Standalone PDF: render it directly, no TeX engine needed.
         * base was already set to the extension-stripped path by the caller. */
        snprintf(pdf, sizeof pdf, "%s", j->pdf_path);
        if (np__access(pdf, 0) == 0)
            pages = pv_render_pdf(j, pdf, j->base, qlog);
        else
            snprintf(j->log, sizeof j->log, "找不到 PDF 文件：%s", pdf);
    } else {
        char qtex[NP_PV_PATH_CAP * 2];
        pv_quote(j->tex_path, qtex, sizeof qtex);
        const char *engine = NULL;
        if (pv_has_tool("xelatex")) engine = "xelatex";
        else if (pv_has_tool("pdflatex")) engine = "pdflatex";
        else if (pv_has_tool("lualatex")) engine = "lualatex";

        if (engine) {
            char cmd[NP_PV_PATH_CAP * 4];
            char qcwd[NP_PV_PATH_CAP * 2];
            pv_quote(j->work_dir, qcwd, sizeof qcwd);
            /* Run from the project directory: multi-file documents resolve
             * \input/\include, .bib files and images relative to the main
             * .tex, not to the editor's launch folder. */
#ifdef _WIN32
            const char *cd = "cd /d";
#else
            const char *cd = "cd";
#endif
            /* Two passes so cross-references and the TOC settle. */
            snprintf(cmd, sizeof cmd,
                     "%s %s && %s -interaction=nonstopmode -halt-on-error -output-directory=%s %s > %s 2>&1",
                     cd, qcwd, engine, qdir, qtex, qlog);
            pv_run(cmd);
            snprintf(cmd, sizeof cmd,
                     "%s %s && %s -interaction=nonstopmode -halt-on-error -output-directory=%s %s >> %s 2>&1",
                     cd, qcwd, engine, qdir, qtex, qlog);
            pv_run(cmd);

            snprintf(pdf, sizeof pdf, "%s.pdf", j->base);
            snprintf(j->pdf_path, sizeof j->pdf_path, "%s", pdf);
            if (np__access(pdf, 0) == 0)
                pages = pv_render_pdf(j, pdf, j->base, qlog);
        } else {
            snprintf(j->log, sizeof j->log,
                     "未找到 LaTeX 引擎。请安装 TinyTeX / TeX Live，或把 xelatex 加入 PATH。");
        }
    }

    if (pages > 0) {
        j->pages = pages;
        j->state = 2;
    } else {
        j->state = 3;
        if (!j->log[0]) {
            char tail[512];
            pv_read_log_tail(log_path, tail, sizeof tail);
            if (tail[0]) snprintf(j->log, sizeof j->log, "%s", tail);
            else snprintf(j->log, sizeof j->log, "预览渲染失败，请查看 PDF/TeX 是否有效。");
        }
    }
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

void np_tex_job_init(np_tex_job *job) {
    memset(job, 0, sizeof *job);
    job->state = 0;
}

int np_tex_state(const np_tex_job *job) { return job ? job->state : 0; }

int np_tex_page_path(const np_tex_job *job, int page, char *buf, size_t cap) {
    if (!job || page < 1 || job->pages < 1 || page > job->pages || !job->base[0])
        return 0;
    snprintf(buf, cap, "%s-%d.png", job->base, page);
    return 1;
}

/* Shared starter for a compile/render job. */
static int pv_tex_start(np_tex_job *job, int mode) {
    pv_tex_ctx *ctx = (pv_tex_ctx *)calloc(1, sizeof *ctx);
    if (!ctx) { job->state = 3; return 0; }
    ctx->job = job;
    ctx->mode = mode;
#ifdef _WIN32
    ctx->th = (HANDLE)_beginthreadex(NULL, 0,
                  (unsigned (__stdcall *)(void *))pv_tex_worker, ctx, 0, NULL);
    if (!ctx->th) { free(ctx); job->state = 3; return 0; }
#else
    if (pthread_create(&ctx->th, NULL, (void *(*)(void *))pv_tex_worker, ctx) != 0) {
        free(ctx); job->state = 3; return 0;
    }
#endif
    job->opaque = ctx;
    return 1;
}

int np_tex_compile(np_tex_job *job, const char *tex_path) {
    if (!job || !tex_path || !*tex_path) return 0;
    if (job->state == 1) return 0;              /* already running */
    memset(job->log, 0, sizeof job->log);
    job->pages = 0;
    job->pdf_path[0] = 0;
    snprintf(job->tex_path, sizeof job->tex_path, "%s", tex_path);
    pv_work_dir(tex_path, job->work_dir, sizeof job->work_dir);
    pv_strip_ext(tex_path, job->base, sizeof job->base);
    job->state = 1;                              /* set before spawning */
    return pv_tex_start(job, 0);
}

int np_tex_render_pdf(np_tex_job *job, const char *pdf_path) {
    if (!job || !pdf_path || !*pdf_path) return 0;
    if (job->state == 1) return 0;
    memset(job->log, 0, sizeof job->log);
    job->pages = 0;
    snprintf(job->pdf_path, sizeof job->pdf_path, "%s", pdf_path);
    snprintf(job->tex_path, sizeof job->tex_path, "%s", pdf_path);
    pv_work_dir(pdf_path, job->work_dir, sizeof job->work_dir);
    pv_strip_ext(pdf_path, job->base, sizeof job->base);
    job->state = 1;
    return pv_tex_start(job, 1);
}

void np_tex_job_free(np_tex_job *job) {
    if (!job || !job->opaque) return;
    pv_tex_ctx *ctx = (pv_tex_ctx *)job->opaque;
#ifdef _WIN32
    if (ctx->th) {
        WaitForSingleObject(ctx->th, INFINITE);
        CloseHandle(ctx->th);
    }
#else
    pthread_join(ctx->th, NULL);
#endif
    free(ctx);
    job->opaque = NULL;
}

/* ------------------------------------------------------------------ */
/* Built-in loopback HTTP server                                       */
/* ------------------------------------------------------------------ */

typedef struct pv_http_ctx {
    np_http_server *server;
#ifdef _WIN32
    SOCKET listen_sock;
    HANDLE th;
#else
    int listen_sock;
    pthread_t th;
#endif
} pv_http_ctx;

/* Minimal URL decode into out (also strips a query string). */
static void pv_url_decode(const char *in, char *out, size_t cap) {
    const char *p = in;
    size_t j = 0;
    while (*p && *p != '?' && j + 1 < cap) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[j++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else if (*p == '+') {
            out[j++] = ' '; p++;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = 0;
}

static const char *pv_mime(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    e++;
    if (pv_ieq(e,"html")||pv_ieq(e,"htm")||pv_ieq(e,"xhtml")) return "text/html; charset=utf-8";
    if (pv_ieq(e,"css"))  return "text/css; charset=utf-8";
    if (pv_ieq(e,"js")||pv_ieq(e,"mjs")) return "text/javascript; charset=utf-8";
    if (pv_ieq(e,"json")) return "application/json; charset=utf-8";
    if (pv_ieq(e,"svg"))  return "image/svg+xml";
    if (pv_ieq(e,"png"))  return "image/png";
    if (pv_ieq(e,"jpg")||pv_ieq(e,"jpeg")) return "image/jpeg";
    if (pv_ieq(e,"gif"))  return "image/gif";
    if (pv_ieq(e,"ico"))  return "image/x-icon";
    if (pv_ieq(e,"woff")||pv_ieq(e,"woff2")) return "font/woff2";
    if (pv_ieq(e,"txt")||pv_ieq(e,"md")) return "text/plain; charset=utf-8";
    if (pv_ieq(e,"wasm")) return "application/wasm";
    return "application/octet-stream";
}

static void pv_send(intptr_t c, const char *status, const char *ctype,
                    const void *body, long len) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, len);
#ifdef _WIN32
    send((SOCKET)c, hdr, n, 0);
    if (len > 0) send((SOCKET)c, (const char *)body, (int)len, 0);
#else
    send((int)c, hdr, (size_t)n, 0);
    if (len > 0) send((int)c, body, (size_t)len, 0);
#endif
}

static void pv_http_serve(pv_http_ctx *ctx) {
    char req[8192];
    while (ctx->server->state == 1) {
#ifdef _WIN32
        SOCKET c = accept(ctx->listen_sock, NULL, NULL);
        if (c == INVALID_SOCKET) break;
#else
        int c = accept(ctx->listen_sock, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
#endif
        intptr_t ci = (intptr_t)c;
        memset(req, 0, sizeof req);
        int got = 0;
        do {
#ifdef _WIN32
            got = recv(c, req + (strlen(req)), (int)(sizeof req - strlen(req) - 1), 0);
#else
            got = (int)recv(c, req + strlen(req), sizeof req - strlen(req) - 1, 0);
#endif
        } while (got > 0 && !strstr(req, "\r\n\r\n") && strlen(req) < sizeof req - 1);

        char path[NP_PV_PATH_CAP], full[NP_PV_PATH_CAP];
        if (strncmp(req, "GET ", 4) == 0) {
            const char *sp = strchr(req + 4, ' ');
            size_t tok = sp ? (size_t)(sp - (req + 4)) : strlen(req + 4);
            char raw[NP_PV_PATH_CAP];
            if (tok >= sizeof raw) tok = sizeof raw - 1;
            memcpy(raw, req + 4, tok); raw[tok] = 0;
            pv_url_decode(raw, path, sizeof path);
        } else {
            path[0] = 0;
        }
        if (!path[0] || strcmp(path, "/") == 0)
            snprintf(path, sizeof path, "/index.html");

        /* Reject traversal: decode, then ensure the path stays under root. */
        int bad = strstr(path, "..") != NULL;
        const char *rel = path;
        while (*rel == '/') rel++;
        if (!bad)
            snprintf(full, sizeof full, "%s/%s", ctx->server->root, rel);

        FILE *f = bad ? NULL : fopen(full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = (char *)malloc(sz > 0 ? (size_t)sz : 1);
            if (buf) {
                size_t nr = fread(buf, 1, (size_t)sz, f);
                pv_send(ci, "200 OK", pv_mime(full), buf, (long)nr);
                free(buf);
            } else {
                pv_send(ci, "500 Internal Server Error", "text/plain; charset=utf-8",
                        "out of memory", 13);
            }
            fclose(f);
        } else {
            const char *msg = "<!doctype html><meta charset=utf-8><title>404</title><h1>404 Not Found</h1>";
            pv_send(ci, "404 Not Found", "text/html; charset=utf-8", msg, (long)strlen(msg));
        }
#ifdef _WIN32
        closesocket(c);
#else
        close(c);
#endif
    }
}

void np_http_init(np_http_server *s) {
    memset(s, 0, sizeof *s);
    s->state = 0;
}

int np_http_running(const np_http_server *s) { return s ? s->state == 1 : 0; }

static void pv_http_worker(void *arg) {
    pv_http_ctx *ctx = (pv_http_ctx *)arg;
    pv_http_serve(ctx);
#ifdef _WIN32
    _endthreadex(0);
#endif
}

int np_http_start(np_http_server *s, const char *root_dir) {
    if (!s || s->state == 1) return s ? s->port : 0;
    snprintf(s->root, sizeof s->root, "%s", root_dir ? root_dir : ".");

    pv_http_ctx *ctx = (pv_http_ctx *)calloc(1, sizeof *ctx);
    if (!ctx) return 0;
    ctx->server = s;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ctx->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_sock == INVALID_SOCKET) { free(ctx); return 0; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                           /* ephemeral */
    if (bind(ctx->listen_sock, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(ctx->listen_sock, 8) == SOCKET_ERROR) {
        closesocket(ctx->listen_sock); free(ctx); return 0;
    }
    int alen = sizeof addr;
    getsockname(ctx->listen_sock, (struct sockaddr *)&addr, &alen);
    s->port = ntohs(addr.sin_port);
#else
    ctx->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_sock < 0) { free(ctx); return 0; }
    int one = 1;
    setsockopt(ctx->listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ctx->listen_sock, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(ctx->listen_sock, 8) != 0) {
        close(ctx->listen_sock); free(ctx); return 0;
    }
    socklen_t alen = sizeof addr;
    getsockname(ctx->listen_sock, (struct sockaddr *)&addr, &alen);
    s->port = ntohs(addr.sin_port);
#endif

    s->opaque = ctx;
    s->state = 1;                                /* after port is set */
#ifdef _WIN32
    ctx->th = (HANDLE)_beginthreadex(NULL, 0,
                  (unsigned (__stdcall *)(void *))pv_http_worker, ctx, 0, NULL);
    if (!ctx->th) { s->state = 0; return 0; }
#else
    if (pthread_create(&ctx->th, NULL, (void *(*)(void *))pv_http_worker, ctx) != 0) {
        s->state = 0; return 0;
    }
#endif
    return s->port;
}

void np_http_stop(np_http_server *s) {
    if (!s || !s->opaque) return;
    s->state = 0;
    pv_http_ctx *ctx = (pv_http_ctx *)s->opaque;
#ifdef _WIN32
    shutdown(ctx->listen_sock, SD_BOTH);
    closesocket(ctx->listen_sock);
    if (ctx->th) { WaitForSingleObject(ctx->th, INFINITE); CloseHandle(ctx->th); }
    WSACleanup();
#else
    shutdown(ctx->listen_sock, SHUT_RDWR);
    close(ctx->listen_sock);
    pthread_join(ctx->th, NULL);
#endif
    free(ctx);
    s->opaque = NULL;
    s->port = 0;
}

/* ------------------------------------------------------------------ */
/* Browser / WebView launch                                            */
/* ------------------------------------------------------------------ */

void np_open_url(const char *url) {
    if (!url || !*url) return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__ANDROID__)
    /* No shell on Android: SDL_OpenURL fires an ACTION_VIEW intent that the
     * system routes to a suitable browser. */
    SDL_OpenURL(url);
#else
#ifdef __APPLE__
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "open '%s'", url);
#else
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
#endif
    system(cmd);
#endif
}

int np_webview_open(const char *url) {
    if (!url || !*url) return 0;
#ifdef _WIN32
    /* Prefer a Chromium browser in chromeless app mode so the preview is a
     * standalone window rather than a tab. Fall back to the default browser. */
    char sysdir[512] = {0};
    char pf[512] = {0};
    GetEnvironmentVariableA(" ProgramFiles", pf, sizeof pf); /* replaced below */
    pf[0] = 0;
    GetEnvironmentVariableA("ProgramFiles", pf, sizeof pf);
    GetEnvironmentVariableA("ProgramFiles(x86)", sysdir, sizeof sysdir);

    const char *candidates[][3] = {
        { "msedge.exe",  "Microsoft\\Edge\\Application\\msedge.exe", "pf" },
        { "msedge.exe",  "Microsoft\\Edge\\Application\\msedge.exe", "pf86" },
        { "chrome.exe",  "Google\\Chrome\\Application\\chrome.exe", "pf" },
        { "chrome.exe",  "Google\\Chrome\\Application\\chrome.exe", "local" },
    };
    char local[512] = {0};
    GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local);
    char params[2048];
    snprintf(params, sizeof params, "--app='%s'", url);
    for (int i = 0; i < 4; i++) {
        char full[1200];
        const char *base = strcmp(candidates[i][2], "pf") == 0 ? pf :
                           strcmp(candidates[i][2], "pf86") == 0 ? sysdir : local;
        if (!base[0]) continue;
        snprintf(full, sizeof full, "%s\\%s", base, candidates[i][1]);
        if (np__access(full, 0) == 0) {
            if ((intptr_t)ShellExecuteA(NULL, "open", full, params, NULL, SW_SHOWNORMAL) > 32) return 1;
        }
    }
    np_open_url(url);
    return 1;
#elif defined(__APPLE__)
    char cmd[2048];
    /* Chrome/Edge in app mode when present, else the default browser. */
    if (access("/Applications/Google Chrome.app", F_OK) == 0) {
        snprintf(cmd, sizeof cmd, "open -a 'Google Chrome' --args --app='%s'", url);
    } else if (access("/Applications/Microsoft Edge.app", F_OK) == 0) {
        snprintf(cmd, sizeof cmd, "open -a 'Microsoft Edge' --args --app='%s'", url);
    } else {
        snprintf(cmd, sizeof cmd, "open '%s'", url);
    }
    system(cmd);
    return 1;
#else
    char cmd[2048];
    if (pv_has_tool("google-chrome"))
        snprintf(cmd, sizeof cmd, "google-chrome --app='%s' >/dev/null 2>&1 &", url);
    else if (pv_has_tool("chromium") || pv_has_tool("chromium-browser"))
        snprintf(cmd, sizeof cmd, "chromium --app='%s' >/dev/null 2>&1 &", url);
    else
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
    system(cmd);
    return 1;
#endif
}
