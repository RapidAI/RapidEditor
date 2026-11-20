#include "np_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Local agents can be slow; cap the wait so a hung CLI never pins a worker
 * thread forever. Overridable at compile time for tests. */
#ifndef NP_AI_TIMEOUT_MS
#define NP_AI_TIMEOUT_MS 180000
#endif

/* Paths used by the AI assistant come from document/workspace names and may
 * contain CJK characters. On Windows the CRT's ANSI fopen()/remove() interpret
 * them in the active code page, so use the UTF-16 variants there. */
static FILE *np_ai_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    if (!path || !mode) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *wpath = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wpath) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n) <= 0) {
        free(wpath);
        return NULL;
    }
    const wchar_t *wmode;
    if (!strcmp(mode, "wb")) wmode = L"wb";
    else if (!strcmp(mode, "rb")) wmode = L"rb";
    else if (!strcmp(mode, "w")) wmode = L"w";
    else wmode = L"r";
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    return f;
#else
    return fopen(path, mode);
#endif
}

static void np_ai_remove_file(const char *path) {
#ifdef _WIN32
    if (!path) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return;
    wchar_t *wpath = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wpath) return;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n) > 0)
        _wremove(wpath);
    free(wpath);
#else
    if (path) remove(path);
#endif
}

bool np_ai_write_context(const char *path, const char *text) {
    if (!path || !text) return false;
    FILE *f = np_ai_fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool np_ai_subst(const char *s, const char *needle, const char *repl,
                 char *out, size_t cap) {
    if (!s || !needle || !repl || !out || cap == 0) return false;
    size_t nn = strlen(needle);
    if (nn == 0) { snprintf(out, cap, "%s", s); return true; }
    size_t o = 0;
    const char *p = s;
    while (*p) {
        const char *hit = strstr(p, needle);
        if (!hit) {
            size_t rest = strlen(p);
            if (o + rest + 1 > cap) return false;
            memcpy(out + o, p, rest + 1);
            o += rest;
            break;
        }
        size_t seg = (size_t)(hit - p);
        size_t rl = strlen(repl);
        if (o + seg + rl + 1 > cap) return false;
        memcpy(out + o, p, seg); o += seg;
        memcpy(out + o, repl, rl); o += rl;
        p = hit + nn;
    }
    out[o] = 0;
    return true;
}

/* Quote a path for the shell. */
static void quote_path(const char *p, char *out, size_t cap) {
    size_t j = 0;
    if (j < cap - 1) out[j++] = '"';
    for (const char *q = p; *q && j + 2 < cap; q++) {
        if (*q == '"' && j + 1 < cap) out[j++] = '\\';
        out[j++] = *q;
    }
    if (j + 1 < cap) out[j++] = '"';
    out[j] = 0;
}

bool np_ai_exec_timeout(const char *tpl, const char *ctx_path, const char *out_path,
                        const char *sys, int timeout_ms, char *err, size_t err_cap) {
    (void)sys;
    if (!tpl || !ctx_path || !out_path) return false;
    if (err && err_cap) err[0] = 0;
    char qfile[2048], qout[2048];
    quote_path(ctx_path, qfile, sizeof qfile);
    quote_path(out_path, qout, sizeof qout);

    char s1[8192], cmd[12288];
    if (!np_ai_subst(tpl, "{file}", qfile, s1, sizeof s1) ||
        !np_ai_subst(s1, "{out}", qout, cmd, sizeof cmd)) {
        if (err && err_cap) snprintf(err, err_cap, "命令模板过长");
        return false;
    }
    /* Remove a stale result so we never show a previous answer on failure. */
    np_ai_remove_file(out_path);

    int rc = -1;
#ifdef _WIN32
    {
        /* Convert the UTF-8 command line to UTF-16 so paths containing CJK
         * characters survive. CreateProcessA + cmd.exe would reinterpret the
         * bytes in the OEM/ANSI code page and mangle them. */
        wchar_t wcmd[16384];
        int wn = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd,
                                     (int)(sizeof(wcmd) / sizeof(wcmd[0])));
        if (wn <= 0) {
            if (err && err_cap) snprintf(err, err_cap, "命令包含非法 UTF-8");
            return false;
        }
        wchar_t wline[17000];
        swprintf(wline, sizeof(wline) / sizeof(wline[0]), L"cmd.exe /d /c %ls", wcmd);

        STARTUPINFOW si; PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessW(NULL, wline, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                           NULL, NULL, &si, &pi)) {
            DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                if (err && err_cap)
                    snprintf(err, err_cap, "AI 命令超时（%d 秒）已终止", timeout_ms / 1000);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                return false;
            }
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            rc = (int)code;
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        } else {
            if (err && err_cap) snprintf(err, err_cap, "无法启动命令进程");
        }
    }
#else
    /* POSIX: system() can't be timed out portably without signals/forks; the
     * agent CLI is expected to terminate on its own here. The Windows path
     * (the primary target) has a hard timeout above. */
    rc = system(cmd);
#endif
    (void)rc;
    FILE *f = np_ai_fopen(out_path, "rb");
    if (!f) {
        if (err && err_cap && !err[0])
            snprintf(err, err_cap, "AI 未产生输出（请检查命令模板与本机 CLI 是否可用）");
        return false;
    }
    char probe[64];
    size_t got = fread(probe, 1, sizeof probe, f);
    fclose(f);
    if (got == 0) {
        if (err && err_cap) snprintf(err, err_cap, "AI 输出为空");
        return false;
    }
    return true;
}

bool np_ai_exec(const char *tpl, const char *ctx_path, const char *out_path,
                const char *sys, char *err, size_t err_cap) {
    return np_ai_exec_timeout(tpl, ctx_path, out_path, sys,
                              NP_AI_TIMEOUT_MS, err, err_cap);
}

long np_ai_read_result(const char *path, char *buf, size_t cap) {
    if (!path || !buf || cap == 0) return -1;
    FILE *f = np_ai_fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}
