/* Online LaTeX template catalogue. See np_templates_online.h. */
#include "np_templates_online.h"
#include "np_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Curated, high-star GitHub LaTeX templates. Branches verified against the
 * GitHub API. These are multi-file projects; our ZIP importer handles them. */
static const np_online_template CURATED[] = {
    { "Awesome CV",          "posquit0/Awesome-CV",             "经典求职简历模板，多语言", "master", 28975 },
    { "中文简历 (billryan)",  "billryan/resume",                 "简约中文 LaTeX 简历", "master", 11574 },
    { "清华大学论文",         "tuna/thuthesis",                  "清华大学学位论文模板", "master", 5493 },
    { "中科院论文",           "mohuangrui/ucasthesis",           "中国科学院大学论文模板", "master", 3926 },
    { "上海交通大学论文",     "sjtug/SJTUThesis",                "上海交大学位论文模板", "master", 3908 },
    { "浙江大学论文",         "TheNetAdmin/zjuthesis",           "浙江大学学位论文模板", "master", 3824 },
    { "LaTeX CV 合集",        "jankapunkt/latexcv",              "多风格 CV/简历模板集合", "master", 3368 },
    { "Pandoc LaTeX 模板",    "Wandmalfarbe/pandoc-latex-template", "Markdown 转 PDF 的 LaTeX 模板", "master", 7306 },
};
#define CURATED_COUNT ((int)(sizeof CURATED / sizeof CURATED[0]))

int np_online_curated(np_online_template *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = cap < CURATED_COUNT ? cap : CURATED_COUNT;
    memcpy(out, CURATED, (size_t)n * sizeof CURATED[0]);
    return n;
}

void np_online_zip_url(const np_online_template *t, char *buf, size_t cap) {
    if (!t || !buf || !cap) return;
    const char *branch = t->branch[0] ? t->branch : "master";
    snprintf(buf, cap, "https://codeload.github.com/%s/zip/refs/heads/%s",
             t->full_name, branch);
}

/* ---- Minimal JSON helper: unescape a JSON string into out --------------- */
static void json_unescape(const char *src, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            i++;
            char e = src[i];
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u':
                    /* \uXXXX: decode BMP; surrogate pairs left as U+FFFD. */
                    if (i + 4 < len) {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = src[i + 1 + k];
                            cp <<= 4;
                            cp |= (unsigned)(isdigit((unsigned char)h) ? h - '0'
                                  : (tolower((unsigned char)h) - 'a' + 10));
                        }
                        i += 4;
                        if (cp >= 0x20 && cp < 0x80) c = (char)cp;
                        else c = '?';   /* non-ASCII handled loosely; names are ASCII */
                    } else c = '?';
                    break;
                default: c = e; break;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
}

/* Find the value of a simple string key within a JSON object slice. */
static void json_str(const char *obj, size_t len, const char *key,
                     char *out, size_t cap) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = obj;
    while (p && p < obj + len) {
        p = strstr(p, pat);
        if (!p || p >= obj + len) return;
        const char *q = p + strlen(pat);
        while (q < obj + len && isspace((unsigned char)*q)) q++;
        if (q < obj + len && *q == ':') {
            q++;
            while (q < obj + len && isspace((unsigned char)*q)) q++;
            if (q < obj + len && *q == '"') {
                const char *s = q + 1;
                const char *e = s;
                while (e < obj + len && *e != '"') {
                    if (*e == '\\' && e + 1 < obj + len) e++;
                    e++;
                }
                if (e <= obj + len) json_unescape(s, (size_t)(e - s), out, cap);
                return;
            }
        }
        p = q;
    }
}

static int json_int(const char *obj, size_t len, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= obj + len) return -1;
    const char *q = p + strlen(pat);
    while (q < obj + len && *q != ':') q++;
    if (q >= obj + len) return -1;
    q++;
    while (q < obj + len && isspace((unsigned char)*q)) q++;
    return atoi(q);
}

/* Extract { ... } objects that are elements of the top-level "items" array. */
int np_online_search(const char *query, np_online_template *out, int cap,
                     volatile long *progress, volatile bool *cancel,
                     char *err, size_t err_cap) {
    if (!query || !*query || !out || cap <= 0) return 0;
    if (err && err_cap) err[0] = 0;
    if (progress) *progress = 0;

    char url[1024];
    /* URL-encode query: safe chars only, percent-encode the rest. */
    char q[256]; size_t j = 0;
    for (const char *p = query; *p && j + 4 < sizeof q; p++) {
        if (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_')
            q[j++] = *p;
        else j += (size_t)snprintf(q + j, sizeof q - j, "%%%02X", (unsigned char)*p);
    }
    q[j] = 0;
    snprintf(url, sizeof url,
             "https://api.github.com/search/repositories?q=%s+in:name,description+language:tex&sort=stars&order=desc&per_page=%d",
             q, cap < 30 ? cap : 30);

    np_net_opts no = {0};
    no.progress = progress; no.cancel = cancel; no.err = err; no.err_cap = err_cap;
    no.timeout_sec = 30;
    char *json = np_net_fetch(url, &no);
    if (!json) return -1;
    if (progress) *progress = 50;

    int n = 0;
    const char *items = strstr(json, "\"items\"");
    if (!items) { free(json); return -1; }

    /* Walk objects by brace depth, only inside the items array. */
    const char *p = strchr(items, '[');
    const char *end = json + strlen(json);
    if (!p) { free(json); return -1; }
    while (p < end && n < cap) {
        if (cancel && *cancel) break;
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        const char *start = p;
        int depth = 0; bool instr = false, esc = false;
        for (; p < end; p++) {
            char c = *p;
            if (instr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') instr = false;
                continue;
            }
            if (c == '"') instr = true;
            else if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) { p++; break; }
            }
        }
        size_t olen = (size_t)(p - start);
        np_online_template *t = &out[n];
        memset(t, 0, sizeof *t);
        json_str(start, olen, "full_name", t->full_name, sizeof t->full_name);
        if (!t->full_name[0]) continue;              /* skip malformed entries */
        json_str(start, olen, "description", t->desc, sizeof t->desc);
        json_str(start, olen, "default_branch", t->branch, sizeof t->branch);
        t->stars = json_int(start, olen, "stargazers_count");
        /* display name = repo part of full_name */
        const char *slash = strrchr(t->full_name, '/');
        snprintf(t->name, sizeof t->name, "%s", slash ? slash + 1 : t->full_name);
        n++;
    }
    free(json);
    if (progress) *progress = 100;
    return n;
}
