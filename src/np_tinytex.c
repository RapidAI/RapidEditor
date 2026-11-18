/* TinyTeX detection + automatic deployment. See np_tinytex.h. */
#include "np_tinytex.h"

#ifdef __ANDROID__
/* No TeX toolchain on Android: inert stubs. */
bool np_tinytex_engine(char *out, size_t cap) { if (out && cap) out[0] = 0; return false; }
bool np_tinytex_installed(void) { return false; }
void np_tinytex_dir(char *out, size_t cap) { if (out && cap) out[0] = 0; }
int  np_tinytex_state(void) { return 0; }
const char *np_tinytex_status_text(void) { return ""; }
bool np_tinytex_install(const char *p, const np_tinytex_opts *o) { (void)p;(void)o; return false; }
void np_tinytex_path_value(char *b, size_t c) { (void)b;(void)c; }
void np_tinytex_reset(void) {}
#else

#include "np_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <io.h>
#define NP_ACCESS _access
#define NP_MKDIR(p) (mkdir((p)) == 0)
#else
#include <unistd.h>
#include <pthread.h>
#define NP_ACCESS access
#define NP_MKDIR(p) (mkdir((p), 0755) == 0)
#endif

/* Pinned TinyTeX release. TinyTeX-0 is the ~24MB/1.4MB base installer that
 * pulls extra packages on demand via tlmgr — fast first download. */
#define NP_TINYTEX_TAG "v2026.11"
#define NP_TT_PATH_CAP 1024

#define NP_TT_IDLE 0
static volatile int g_state = NP_TT_IDLE;
static char g_status[256] = "";

static void tt_status(const char *s) {
    snprintf(g_status, sizeof g_status, "%s", s);
}

static void tt_err(const np_tinytex_opts *o, const char *m) {
    if (o && o->err && o->err_cap) snprintf(o->err, o->err_cap, "%s", m);
    snprintf(g_status, sizeof g_status, "%s", m);
}

/* App-private config dir — same convention as np_config_dir in rapideditor.c. */
void np_tinytex_dir(char *out, size_t cap) {
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) base = ".";
    snprintf(out, cap, "%s\\RapidEditor\\TinyTeX", base);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/rapideditor/tinytex", xdg);
    else {
        const char *h = getenv("HOME");
        if (!h || !*h) h = ".";
        snprintf(out, cap, "%s/.local/share/rapideditor/tinytex", h);
    }
#endif
}

static void tt_bin_dir(char *out, size_t cap) {
    char root[NP_TT_PATH_CAP];
    np_tinytex_dir(root, sizeof root);
#ifdef _WIN32
    snprintf(out, cap, "%s\\bin\\windows", root);
#else
    /* LaTeX bin dir is <root>/bin/<arch> (e.g. x86_64-linux, aarch64-linux,
     * universal-darwin). Find the single child of bin/. */
    char bin[NP_TT_PATH_CAP];
    snprintf(bin, sizeof bin, "%s/bin", root);
    snprintf(out, cap, "%s", bin);
#ifdef __APPLE__
    const char *guess = "universal-darwin";
#else
    const char *guess = "x86_64-linux";
#endif
    char cand[NP_TT_PATH_CAP];
    snprintf(cand, sizeof cand, "%s/%s", bin, guess);
    if (NP_ACCESS(cand, 0) == 0) snprintf(out, cap, "%s", cand);
    else {
        /* scan bin/* for a directory containing xelatex */
        char cmd[NP_TT_PATH_CAP * 2];
        snprintf(cmd, sizeof cmd, "ls '%s' 2>/dev/null", bin);
        FILE *f = popen(cmd, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!line[0]) continue;
                snprintf(cand, sizeof cand, "%s/%s", bin, line);
                char exe[NP_TT_PATH_CAP];
                snprintf(exe, sizeof exe, "%s/xelatex", cand);
                if (NP_ACCESS(exe, 0) == 0) { snprintf(out, cap, "%s", cand); break; }
            }
            pclose(f);
        }
    }
#endif
}

static bool tt_is_engine(const char *path) {
    return NP_ACCESS(path, 0) == 0;
}

bool np_tinytex_installed(void) {
    char root[NP_TT_PATH_CAP], token[NP_TT_PATH_CAP];
    np_tinytex_dir(root, sizeof root);
#ifdef _WIN32
    snprintf(token, sizeof token, "%s\\.tinytex", root);
#else
    snprintf(token, sizeof token, "%s/.tinytex", root);
#endif
    char bin[NP_TT_PATH_CAP], exe[NP_TT_PATH_CAP];
    tt_bin_dir(bin, sizeof bin);
#ifdef _WIN32
    snprintf(exe, sizeof exe, "%s\\xelatex.exe", bin);
#else
    snprintf(exe, sizeof exe, "%s/xelatex", bin);
#endif
    return NP_ACCESS(token, 0) == 0 && NP_ACCESS(exe, 0) == 0;
}

/* Look for a command on PATH. `where`/`command -v` via popen, but that can
 * flash a console on Windows; instead scan PATH entries directly. */
static bool tt_on_path(const char *exe, char *out, size_t cap) {
    const char *path = getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char *sep = ";"; const char *ext = ".exe";
#else
    const char *sep = ":"; const char *ext = "";
#endif
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", path);
#ifdef _WIN32
    char *ctx = NULL;
    for (char *dir = strtok_s(copy, sep, &ctx); dir;
         dir = strtok_s(NULL, sep, &ctx)) {
#else
    char *ctx = NULL;
    for (char *dir = strtok_r(copy, sep, &ctx); dir;
         dir = strtok_r(NULL, sep, &ctx)) {
#endif
        char cand[NP_TT_PATH_CAP];
        snprintf(cand, sizeof cand, "%s%c%s%s", dir,
#ifdef _WIN32
                 '\\',
#else
                 '/',
#endif
                 exe, ext);
        if (tt_is_engine(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    return false;
}

bool np_tinytex_engine(char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    /* PATH first — a full TeX Live install should win over our private copy. */
    if (tt_on_path("xelatex", out, cap) || tt_on_path("pdflatex", out, cap))
        return true;
    if (np_tinytex_installed()) {
        char bin[NP_TT_PATH_CAP];
        tt_bin_dir(bin, sizeof bin);
#ifdef _WIN32
        snprintf(out, cap, "%s\\xelatex.exe", bin);
        if (!tt_is_engine(out)) snprintf(out, cap, "%s\\pdflatex.exe", bin);
#else
        snprintf(out, cap, "%s/xelatex", bin);
        if (!tt_is_engine(out)) snprintf(out, cap, "%s/pdflatex", bin);
#endif
        if (tt_is_engine(out)) return true;
    }
    if (out && cap) out[0] = 0;
    return false;
}

void np_tinytex_path_value(char *buf, size_t cap) {
    if (np_tinytex_installed()) {
        char bin[NP_TT_PATH_CAP];
        tt_bin_dir(bin, sizeof bin);
        char joined[NP_TT_PATH_CAP * 2];
        snprintf(joined, sizeof joined, "%s%c%s",
#ifdef _WIN32
                 bin, ';',
#else
                 bin, ':',
#endif
                 buf);
        snprintf(buf, cap, "%s", joined);
    }
}

int  np_tinytex_state(void) { return g_state; }
const char *np_tinytex_status_text(void) { return g_status; }
void np_tinytex_reset(void) { if (g_state == 3) g_state = NP_TT_IDLE; }

/* Build the download URL for this platform's TinyTeX-0 base package. */
static bool tt_url(char *out, size_t cap) {
#ifdef _WIN32
    snprintf(out, cap,
             "https://github.com/rstudio/tinytex-releases/releases/download/%s/TinyTeX-0-windows-%s.exe",
             NP_TINYTEX_TAG, NP_TINYTEX_TAG);
#elif defined(__APPLE__)
    snprintf(out, cap,
             "https://github.com/rstudio/tinytex-releases/releases/download/%s/TinyTeX-0-darwin-%s.tar.xz",
             NP_TINYTEX_TAG, NP_TINYTEX_TAG);
#else
    #if defined(__aarch64__)
    const char *arch = "arm64";
    #else
    const char *arch = "x86_64";
    #endif
    snprintf(out, cap,
             "https://github.com/rstudio/tinytex-releases/releases/download/%s/TinyTeX-0-linux-%s-%s.tar.xz",
             NP_TINYTEX_TAG, arch, NP_TINYTEX_TAG);
#endif
    return true;
}

bool np_tinytex_install(const char *packages, const np_tinytex_opts *o) {
    if (g_state == 1) { tt_err(o, "安装正在进行中"); return false; }
    g_state = 1;
    tt_status("准备下载 TinyTeX…");

    char root[NP_TT_PATH_CAP], tmp[NP_TT_PATH_CAP], url[NP_TT_PATH_CAP];
    np_tinytex_dir(root, sizeof root);
    /* ensure parent directory exists */
    char parent[NP_TT_PATH_CAP];
    snprintf(parent, sizeof parent, "%s", root);
    char *pslash = NULL;
    for (char *p = parent + 1; *p; p++) if (*p == '\\' || *p == '/') pslash = p;
    if (pslash) { *pslash = 0; NP_MKDIR(parent); }
    NP_MKDIR(root);

    tt_url(url, sizeof url);
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s\\tinytex-base.exe", root);
#else
    snprintf(tmp, sizeof tmp, "%s/tinytex-base.tar.xz", root);
#endif
    if (o && o->progress) *o->progress = 0;
    tt_status("正在下载 TinyTeX 基础包…");
    np_net_opts no = {0};
    no.progress = o ? o->progress : NULL;
    no.cancel = o ? o->cancel : NULL;
    no.err = o ? o->err : NULL;
    no.err_cap = o ? o->err_cap : 0;
    no.timeout_sec = 600;
    if (!np_net_download(url, tmp, &no)) {
        tt_err(o, (o && o->err && o->err[0]) ? o->err : "下载 TinyTeX 失败");
        g_state = 3; return false;
    }
    if (o && o->cancel && *o->cancel) { tt_err(o, "已取消"); g_state = 3; return false; }

    bool ok = false;
    tt_status("正在安装 TinyTeX…");
#ifdef _WIN32
    /* Inno Setup silent install into our private root: /S /D=<path> (no quotes). */
    char cmd[NP_TT_PATH_CAP * 2];
    snprintf(cmd, sizeof cmd, "\"%s\" /S /D=%s", tmp, root);
    int rc = system(cmd);
    ok = (rc == 0);
    if (!ok) tt_err(o, "TinyTeX 静默安装失败");
#else
    /* Unpack the tar.xz into root (archive contains bin/, texmf-dist/, ...). */
    char cmd[NP_TT_PATH_CAP * 3];
    snprintf(cmd, sizeof cmd, "tar -xJf '%s' -C '%s' --strip-components=1", tmp, root);
    int rc = system(cmd);
    ok = (rc == 0);
    if (!ok) tt_err(o, "解包 TinyTeX 失败（需要 tar）");
#endif
    remove(tmp);
    if (!ok) { g_state = 3; return false; }

    /* Drop the .tinytex token so future runs detect the private install. */
    char token[NP_TT_PATH_CAP];
#ifdef _WIN32
    snprintf(token, sizeof token, "%s\\.tinytex", root);
#else
    snprintf(token, sizeof token, "%s/.tinytex", root);
#endif
    FILE *tf = fopen(token, "wb");
    if (tf) { fputs("1", tf); fclose(tf); }

    /* Install a few common packages so Chinese/beam templates compile first try. */
    if (packages && *packages) {
        tt_status("正在安装常用宏包…");
        char bin[NP_TT_PATH_CAP], tcmd[NP_TT_PATH_CAP * 3];
        tt_bin_dir(bin, sizeof bin);
#ifdef _WIN32
        snprintf(tcmd, sizeof tcmd, "\"%s\\tlmgr.bat\" install %s > nul 2>&1", bin, packages);
#else
        snprintf(tcmd, sizeof tcmd, "'%s/tlmgr' install %s >/dev/null 2>&1", bin, packages);
#endif
        system(tcmd); /* best-effort: on-demand tlmgr still covers the rest */
    }

    if (o && o->progress) *o->progress = 100;
    tt_status("TinyTeX 安装完成");
    g_state = 2;
    return true;
}

#endif /* !__ANDROID__ */
