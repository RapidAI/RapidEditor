/* Cross-platform HTTPS downloader. See np_net.h for the contract. */
#include "np_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void net_err(const np_net_opts *o, const char *msg) {
    if (o && o->err && o->err_cap) snprintf(o->err, o->err_cap, "%s", msg);
}
static void net_prog(const np_net_opts *o, long pct) {
    if (o && o->progress) *o->progress = pct;
}
static bool net_cancelled(const np_net_opts *o) {
    return o && o->cancel && *o->cancel;
}
static int net_timeout(const np_net_opts *o) {
    return o && o->timeout_sec > 0 ? o->timeout_sec : 120;
}

#if defined(__ANDROID__)
/* Online templates / TinyTeX are desktop-only; inert stubs on Android. */
bool np_net_download(const char *url, const char *dest, const np_net_opts *o) {
    (void)url; (void)dest; if (o && o->err && o->err_cap) o->err[0] = 0; return false;
}
char *np_net_fetch(const char *url, const np_net_opts *o) { (void)url; (void)o; return NULL; }

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

/* One HTTP transaction, following up to 10 redirects (GitHub releases 302 to
 * objects.githubusercontent.com, which is a different host). `out` receives
 * the body; pass NULL to discard. Returns true on HTTP 2xx with data. */
static bool winhttp_fetch(const char *url, FILE *out, const np_net_opts *o) {
    bool ok = false;
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    char cur[4096];
    snprintf(cur, sizeof cur, "%s", url);

    hs = WinHttpOpen(L"RapidEditor/1.2 (LaTeX template downloader)",
                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { net_err(o, "无法初始化网络"); return false; }
    {   /* honour the system proxy (needed on many CN/corporate networks) */
        WINHTTP_PROXY_INFO pi = {0};
        if (WinHttpGetDefaultProxyConfiguration(&pi))
            WinHttpSetOption(hs, WINHTTP_OPTION_PROXY, &pi, sizeof pi);
    }
    DWORD tmo = (DWORD)net_timeout(o) * 1000;
    WinHttpSetTimeouts(hs, tmo, tmo, tmo, tmo);

    for (int hop = 0; hop <= 10; hop++) {
        /* GitHub URLs are plain https://host/path; parse them by hand rather
         * than fight WinHttpCrackUrl's A/W macro mapping across SDK versions. */
        const char *scheme = "https";
        bool secure = true;
        const char *rest = cur;
        if (!strncmp(rest, "https://", 8)) rest += 8;
        else if (!strncmp(rest, "http://", 7)) { rest += 7; scheme = "http"; secure = false; }
        else { net_err(o, "仅支持 http/https URL"); goto done; }
        char host[512], path[8192];
        INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        {
            const char *slash = strchr(rest, '/');
            size_t hl = slash ? (size_t)(slash - rest) : strlen(rest);
            if (hl == 0 || hl >= sizeof host) { net_err(o, "URL 主机名无效"); goto done; }
            memcpy(host, rest, hl); host[hl] = 0;
            char *colon = strrchr(host, ':');
            if (colon && colon[1]) { port = (INTERNET_PORT)atoi(colon + 1); *colon = 0; }
            snprintf(path, sizeof path, "%s", slash ? slash : "/");
        }
        (void)scheme;

        WCHAR whost[512], wpath[8192];
        MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 512);
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 8192);
        hc = WinHttpConnect(hs, whost, port, 0);
        if (!hc) { net_err(o, "无法连接服务器"); goto done; }
        hr = WinHttpOpenRequest(hc, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hr) { net_err(o, "无法创建请求"); goto done; }
        if (!WinHttpSendRequest(hr, L"Accept: */*", -1, NULL, 0, 0, 0) ||
            !WinHttpReceiveResponse(hr, NULL)) {
            net_err(o, "发送请求失败（检查网络/代理）"); goto done;
        }

        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &sz, NULL);
        if (status >= 300 && status < 400) {
            char loc[4096]; DWORD lsz = sizeof loc;
            if (hop < 10 && WinHttpQueryHeaders(hr, WINHTTP_QUERY_LOCATION, NULL, loc, &lsz, NULL)) {
                WinHttpCloseHandle(hr); hr = NULL;
                WinHttpCloseHandle(hc); hc = NULL;
                if (loc[0] == '/') { /* relative redirect */
                    snprintf(cur, sizeof cur, "%s://%s%s", scheme, host, loc);
                } else snprintf(cur, sizeof cur, "%s", loc);
                continue;
            }
        }
        if (status < 200 || status >= 300) {
            char m[96]; snprintf(m, sizeof m, "HTTP 状态 %lu", (unsigned long)status);
            net_err(o, m); goto done;
        }

        DWORD total = 0; size_t got = 0; sz = sizeof total;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &total, &sz, NULL);
        for (;;) {
            if (net_cancelled(o)) { net_err(o, "已取消"); goto done; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) break;
            DWORD read = 0;
            char chunk[16384];
            if (!WinHttpReadData(hr, chunk,
                                 avail < sizeof chunk ? avail : (DWORD)sizeof chunk, &read) || !read)
                break;
            if (out && fwrite(chunk, 1, read, out) != read) {
                net_err(o, "写入文件失败"); goto done;
            }
            got += read;
            if (total > 0) {
                long pct = (long)(got * 100 / total);
                if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                net_prog(o, pct);
            }
        }
        if (got == 0) { net_err(o, "服务器返回空内容"); goto done; }
        net_prog(o, 100);
        ok = true;
        break;
    }
    if (!ok && o && o->err && !o->err[0]) net_err(o, "重定向次数过多");

done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    return ok;
}

bool np_net_download(const char *url, const char *dest, const np_net_opts *o) {
    if (!url || !dest) return false;
    FILE *f = fopen(dest, "wb");
    if (!f) { net_err(o, "无法创建目标文件"); return false; }
    bool ok = winhttp_fetch(url, f, o);
    fclose(f);
    if (!ok) remove(dest);
    return ok;
}

char *np_net_fetch(const char *url, const np_net_opts *o) {
    if (!url) return NULL;
    char dir[MAX_PATH], tmpfile[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    if (!GetTempFileNameA(dir, "re", 0, tmpfile)) return NULL;
    if (!np_net_download(url, tmpfile, o)) { remove(tmpfile); return NULL; }
    FILE *f = fopen(tmpfile, "rb");
    if (!f) { remove(tmpfile); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf) { size_t r = fread(buf, 1, (size_t)n, f); buf[r] = 0; }
    fclose(f);
    remove(tmpfile);
    return buf;
}

#else /* POSIX: use the curl CLI (present on all target desktops) */

#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

static bool posix_fetch(const char *url, const char *dest, const np_net_opts *o, bool quiet) {
    if (!url || !dest) return false;
    char cmd[8192];
    /* Percent-encode unsafe URL bytes to avoid shell injection. */
    char urlq[4096]; size_t j = 0;
    for (const char *p = url; *p && j + 4 < sizeof urlq; p++) {
        if (isalnum((unsigned char)*p) || strchr("-._~:/?#[]@!$&'()*+,;=%", *p))
            urlq[j++] = *p;
        else j += (size_t)snprintf(urlq + j, sizeof urlq - j, "%%%02X", (unsigned char)*p);
    }
    urlq[j] = 0;
    snprintf(cmd, sizeof cmd,
             "curl -sSL --fail --connect-timeout 15 --max-time %d --retry 3 %s -o '%s' '%s'",
             net_timeout(o), quiet ? "" : "", dest, urlq);
    int rc = system(cmd);
    net_prog(o, rc == 0 ? 100 : 0);
    if (rc != 0) { net_err(o, "下载失败（检查网络或代理）"); return false; }
    struct stat st;
    if (stat(dest, &st) != 0 || st.st_size == 0) { net_err(o, "服务器返回空内容"); return false; }
    return true;
}

bool np_net_download(const char *url, const char *dest, const np_net_opts *o) {
    return posix_fetch(url, dest, o, false);
}

char *np_net_fetch(const char *url, const np_net_opts *o) {
    char tmpl[] = "/tmp/re-net-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);
    if (!posix_fetch(url, tmpl, o, true)) { remove(tmpl); return NULL; }
    FILE *f = fopen(tmpl, "rb");
    if (!f) { remove(tmpl); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf) { size_t r = fread(buf, 1, (size_t)n, f); buf[r] = 0; }
    fclose(f);
    remove(tmpl);
    return buf;
}

#endif

/* ---- JSON POST (maclaw device-authorization flow) ---------------------- */

#if defined(__ANDROID__)
char *np_net_post_json(const char *url, const char *bearer, const char *body,
                       const np_net_opts *o, int *status_out) {
    (void)url; (void)bearer; (void)body; (void)o;
    if (status_out) *status_out = 0;
    net_err(o, "Android 不支持此操作");
    return NULL;
}

char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *o, int *status_out) {
    (void)url; (void)bearer; (void)o;
    if (status_out) *status_out = 0;
    net_err(o, "Android 不支持此操作");
    return NULL;
}

#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
static char *np_net_request_win(const char *url, const char *bearer,
                                const char *method, const char *body,
                                const np_net_opts *o, int *status_out) {
    char *result = NULL;
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    if (status_out) *status_out = 0;

    const char *rest = url;
    bool secure = true;
    if (!strncmp(rest, "https://", 8)) rest += 8;
    else if (!strncmp(rest, "http://", 7)) { rest += 7; secure = false; }
    else { net_err(o, "仅支持 http/https URL"); return NULL; }
    char host[512], path[8192];
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    const char *slash = strchr(rest, '/');
    size_t hl = slash ? (size_t)(slash - rest) : strlen(rest);
    if (hl == 0 || hl >= sizeof host) { net_err(o, "URL 主机名无效"); return NULL; }
    memcpy(host, rest, hl); host[hl] = 0;
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) { port = (INTERNET_PORT)atoi(colon + 1); *colon = 0; }
    snprintf(path, sizeof path, "%s", slash ? slash : "/");

    hs = WinHttpOpen(L"RapidEditor/1.2 (maclaw auth)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { net_err(o, "无法初始化网络"); return NULL; }
    {
        WINHTTP_PROXY_INFO pi = {0};
        if (WinHttpGetDefaultProxyConfiguration(&pi))
            WinHttpSetOption(hs, WINHTTP_OPTION_PROXY, &pi, sizeof pi);
    }
    DWORD tmo = (DWORD)net_timeout(o) * 1000;
    WinHttpSetTimeouts(hs, tmo, tmo, tmo, tmo);

    WCHAR whost[512], wpath[8192];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 512);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 8192);
    hc = WinHttpConnect(hs, whost, port, 0);
    if (!hc) { net_err(o, "无法连接服务器"); goto done; }
    WCHAR wmethod[16];
    MultiByteToWideChar(CP_UTF8, 0, method, -1, wmethod, 16);
    hr = WinHttpOpenRequest(hc, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { net_err(o, "无法创建请求"); goto done; }

    if (body) {
        WCHAR wheaders[4096];
        const char *base = "Content-Type: application/json\r\nAccept: application/json";
        MultiByteToWideChar(CP_UTF8, 0, base, -1, wheaders, 4096);
        WinHttpAddRequestHeaders(hr, wheaders, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (bearer && *bearer) {
        char h[1024]; WCHAR wh[1024];
        snprintf(h, sizeof h, "Authorization: Bearer %s", bearer);
        MultiByteToWideChar(CP_UTF8, 0, h, -1, wh, 1024);
        WinHttpAddRequestHeaders(hr, wh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    DWORD body_len = body ? (DWORD)strlen(body) : 0;
    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                            body_len, body_len, 0) ||
        !WinHttpReceiveResponse(hr, NULL)) {
        net_err(o, "发送请求失败（检查网络/代理）"); goto done;
    }

    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &sz, NULL);
    if (status_out) *status_out = (int)status;
    if (status < 200 || status >= 300) {
        char m[96]; snprintf(m, sizeof m, "HTTP 状态 %lu", (unsigned long)status);
        net_err(o, m); goto done;
    }

    size_t cap = 4096, used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { net_err(o, "内存不足"); goto done; }
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) break;
        if (used + avail + 1 > cap) {
            while (used + avail + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); net_err(o, "内存不足"); goto done; }
            buf = nb;
        }
        DWORD read = 0;
        if (!WinHttpReadData(hr, buf + used, avail, &read) || !read) break;
        used += read;
    }
    buf[used] = 0;
    result = buf;
    net_prog(o, 100);

done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    return result;
}
char *np_net_post_json(const char *url, const char *bearer, const char *body,
                       const np_net_opts *o, int *status_out) {
    return np_net_request_win(url, bearer, "POST", body, o, status_out);
}
char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *o, int *status_out) {
    return np_net_request_win(url, bearer, "GET", NULL, o, status_out);
}

#else
char *np_net_post_json(const char *url, const char *bearer, const char *body,
                       const np_net_opts *o, int *status_out) {
    if (status_out) *status_out = 0;
    if (!url || !body) { net_err(o, "参数缺失"); return NULL; }
    net_err(o, "POSIX 平台请使用 curl CLI 版本");
    (void)bearer;
    return NULL;
}
char *np_net_get_json(const char *url, const char *bearer,
                      const np_net_opts *o, int *status_out) {
    if (status_out) *status_out = 0;
    if (!url) { net_err(o, "参数缺失"); return NULL; }
    net_err(o, "POSIX 平台请使用 curl CLI 版本");
    (void)bearer;
    return NULL;
}
#endif
