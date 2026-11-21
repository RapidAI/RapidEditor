#include "np_openai.h"
#include "np_net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool np_openai_json_get_string(const char *json, const char *key,
                               char *out, size_t cap) {
    if (!json || !key || !out || cap == 0) return false;
    out[0] = 0;
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    p++;
    const char *start = p;
    for (; *p; p++) {
        if (*p == '\\') { if (p[1]) p++; continue; }
        if (*p == '"') break;
    }
    if (*p != '"') return false;
    /* Reuse the maclaw unescaper through its public JSON helper semantics:
     * do a local minimal unescape (\" \\ \/ \n \r \t \uXXXX). */
    size_t o = 0;
    for (const char *s = start; s < p && o + 1 < cap; s++) {
        if (*s != '\\' || s + 1 >= p) { out[o++] = *s; continue; }
        s++;
        switch (*s) {
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'u': {
                if (s + 4 >= p) break;
                unsigned cp = 0;
                for (int k = 1; k <= 4 && s + k < p; k++) {
                    char c = s[k];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                }
                s += 4;
                if (cp < 0x80) out[o++] = (char)cp;
                else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out[o++] = *s; break;
        }
    }
    out[o] = 0;
    return true;
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (!s) s = "";
    for (const char *p = s; *p && o + 6 < cap; p++) {
        switch (*p) {
            case '"': memcpy(out + o, "\\\"", 2); o += 2; break;
            case '\\': memcpy(out + o, "\\\\", 2); o += 2; break;
            case '\n': memcpy(out + o, "\\n", 2); o += 2; break;
            case '\r': memcpy(out + o, "\\r", 2); o += 2; break;
            case '\t': memcpy(out + o, "\\t", 2); o += 2; break;
            default: out[o++] = *p; break;
        }
    }
    out[o] = 0;
}

/* Normalize a base URL: trim, add https scheme, strip trailing slashes and a
 * trailing /v1 so we can append /v1/chat/completions ourselves. */
static void normalize_base(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    /* trim spaces */
    const char *s = in ? in : "";
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '/')) n--;
    if (n >= 3 && !strncmp(s + n - 3, "/v1", 3)) n -= 3;
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    char tmp[512];
    if (n >= sizeof tmp) { out[0] = 0; return; }
    memcpy(tmp, s, n); tmp[n] = 0;
    if (strncmp(tmp, "http://", 7) && strncmp(tmp, "https://", 8))
        snprintf(out, cap, "https://%s", tmp);
    else
        snprintf(out, cap, "%s", tmp);
}

bool np_openai_chat(const char *base_url, const char *api_key,
                    const char *model, const char *prompt,
                    char *out, size_t out_cap,
                    char *err, size_t err_cap) {
    if (out && out_cap) out[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!base_url || !model || !prompt || !out || !out_cap) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return false;
    }
    char base[512];
    normalize_base(base_url, base, sizeof base);
    if (!base[0]) {
        if (err && err_cap) snprintf(err, err_cap, "服务地址无效");
        return false;
    }

    size_t esc_cap = strlen(prompt) * 6 + 64;
    char *escaped = (char *)malloc(esc_cap);
    if (!escaped) {
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    json_escape(prompt, escaped, esc_cap);
    size_t body_cap = strlen(escaped) + strlen(model) + 256;
    char *body = (char *)malloc(body_cap);
    if (!body) {
        free(escaped);
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    snprintf(body, body_cap,
             "{\"model\":\"%s\",\"stream\":false,"
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             model, escaped);
    free(escaped);

    char url[1200];
    snprintf(url, sizeof url, "%s/v1/chat/completions", base);

    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    if (err) { opts.err = err; opts.err_cap = err_cap; }
    opts.timeout_sec = 120;
    int status = 0;
    char *resp = np_net_post_json(url, api_key && api_key[0] ? api_key : NULL,
                                  body, &opts, &status);
    free(body);
    if (!resp) {
        if (err && err_cap && !err[0])
            snprintf(err, err_cap, "LLM 服务请求失败（HTTP %d）", status);
        return false;
    }
    bool ok = np_openai_json_get_string(resp, "content", out, out_cap) && out[0];
    if (!ok && err && err_cap)
        snprintf(err, err_cap, "LLM 服务未返回文本内容（HTTP %d）", status);
    free(resp);
    return ok;
}

int np_openai_list_models(const char *base_url, const char *api_key,
                          char *out_csv, size_t cap,
                          char *err, size_t err_cap) {
    if (out_csv && cap) out_csv[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!base_url || !out_csv || !cap) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return -1;
    }
    char base[512];
    normalize_base(base_url, base, sizeof base);
    if (!base[0]) {
        if (err && err_cap) snprintf(err, err_cap, "服务地址无效");
        return -1;
    }
    char url[1200];
    snprintf(url, sizeof url, "%s/v1/models", base);
    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    if (err) { opts.err = err; opts.err_cap = err_cap; }
    opts.timeout_sec = 30;
    int status = 0;
    char *resp = np_net_get_json(url, api_key && api_key[0] ? api_key : NULL,
                                 &opts, &status);
    if (!resp) {
        if (err && err_cap && !err[0])
            snprintf(err, err_cap, "获取模型列表失败（HTTP %d）", status);
        return -1;
    }
    /* Collect every "id":"..." occurrence. */
    int count = 0;
    size_t o = 0;
    const char *p = resp;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        char id[128];
        if (!np_openai_json_get_string(p, "id", id, sizeof id) || !id[0]) {
            p += 4;
            continue;
        }
        size_t idlen = strlen(id);
        if (o + idlen + 2 >= cap) break;
        if (count > 0) out_csv[o++] = ',';
        memcpy(out_csv + o, id, idlen);
        o += idlen;
        out_csv[o] = 0;
        count++;
        p += 4;
    }
    free(resp);
    if (count == 0 && err && err_cap)
        snprintf(err, err_cap, "服务未返回模型列表（HTTP %d）", status);
    return count == 0 ? -1 : count;
}
