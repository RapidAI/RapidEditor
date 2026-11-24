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
                /* A surrogate pair (\uD83D\uDE00) is one code point, not two
                 * 3-byte sequences. Combining it here avoids invalid UTF-8
                 * when a provider serializes content with ensure_ascii. */
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    s + 6 < p && s[1] == '\\' && s[2] == 'u') {
                    unsigned lo = 0;
                    for (int k = 3; k <= 6 && s + k < p; k++) {
                        char c = s[k];
                        lo <<= 4;
                        if (c >= '0' && c <= '9') lo |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') lo |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') lo |= (unsigned)(c - 'A' + 10);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s += 6;                 /* consume the low surrogate */
                    }
                }
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[o++] = (char)(0xF0 | (cp >> 18));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
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

/* Optional sampling parameters; zero = omit from the JSON body. */
static float g_temperature = 0.f;
static int   g_max_tokens = 0;

void np_openai_set_params(float temperature, int max_tokens) {
    g_temperature = temperature;
    g_max_tokens = max_tokens;
}

/* Append sampling params when configured. Must be called with extra body
 * headroom (>= 64 bytes) reserved by the caller. */
static void write_params(char *body, size_t cap, size_t *used) {
    size_t o = strlen(body);
    if (g_temperature > 0.f)
        o += (size_t)snprintf(body + o, cap - o, "\"temperature\":%.2f,",
                              (double)g_temperature);
    if (g_max_tokens > 0)
        o += (size_t)snprintf(body + o, cap - o, "\"max_tokens\":%d,",
                              g_max_tokens);
    (void)used;
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
    size_t body_cap = strlen(escaped) + strlen(model) + 384;
    char *body = (char *)malloc(body_cap);
    if (!body) {
        free(escaped);
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    snprintf(body, body_cap, "{\"model\":\"%s\",\"stream\":false,", model);
    write_params(body, body_cap, NULL);
    size_t bo = strlen(body);
    snprintf(body + bo, body_cap - bo,
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             escaped);
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

/* ---- Streaming SSE chat completions ---------------------------------- */

typedef struct np_sse_state {
    char *out;
    size_t cap, used;
    char line[16384];
    size_t line_len;
    volatile bool *cancel;
    np_llm_delta_cb on_delta;
    void *user;
    bool done, truncated;
} np_sse_state;

static bool np_sse_append(np_sse_state *s, const char *text) {
    size_t n = strlen(text);
    if (!n) return true;
    if (s->used + n + 1 > s->cap) {
        n = s->cap > s->used + 1 ? s->cap - s->used - 1 : 0;
        s->truncated = true;
    }
    if (!n) return false;
    memcpy(s->out + s->used, text, n);
    s->used += n;
    s->out[s->used] = 0;
    if (s->on_delta) return s->on_delta(text, n, s->user);
    return true;
}

static bool np_sse_line(np_sse_state *s, const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "data:", 5)) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
    } else if (*p != '{') {
        return true;                 /* comments / keepalive */
    }
    if (!strcmp(p, "[DONE]")) { s->done = true; return true; }
    if (*p != '{') return true;
    char delta[16384];
    if (!np_openai_json_get_string(p, "content", delta, sizeof delta) || !delta[0])
        return true;
    return np_sse_append(s, delta);
}

static bool np_sse_chunk_cb(const char *chunk, size_t len, void *user) {
    np_sse_state *s = (np_sse_state *)user;
    for (size_t i = 0; i < len; i++) {
        if (s->cancel && *s->cancel) return false;
        char c = chunk[i];
        if (c == '\n') {
            s->line[s->line_len] = 0;
            bool keep = np_sse_line(s, s->line);
            s->line_len = 0;
            if (!keep) return false;
        } else if (c != '\r') {
            if (s->line_len + 1 >= sizeof s->line) {
                s->line_len = 0;      /* malformed oversized line: drop it */
                s->truncated = true;
            } else {
                s->line[s->line_len++] = c;
            }
        }
    }
    return true;
}

bool np_openai_chat_stream(const char *base_url, const char *api_key,
                           const char *model, const char *prompt,
                           char *out, size_t out_cap,
                           char *err, size_t err_cap,
                           volatile bool *cancel,
                           np_llm_delta_cb on_delta, void *user) {
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
    size_t body_cap = strlen(escaped) + strlen(model) + 384;
    char *body = (char *)malloc(body_cap);
    if (!body) {
        free(escaped);
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    snprintf(body, body_cap, "{\"model\":\"%s\",\"stream\":true,", model);
    write_params(body, body_cap, NULL);
    size_t bo = strlen(body);
    snprintf(body + bo, body_cap - bo,
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             escaped);
    free(escaped);

    char url[1200];
    snprintf(url, sizeof url, "%s/v1/chat/completions", base);
    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    if (err) { opts.err = err; opts.err_cap = err_cap; }
    opts.cancel = cancel;
    opts.timeout_sec = 180;

    np_sse_state st;
    memset(&st, 0, sizeof st);
    st.out = out; st.cap = out_cap;
    st.cancel = cancel; st.on_delta = on_delta; st.user = user;

    int status = 0;
    bool ok = np_net_post_json_stream(url,
                                      api_key && api_key[0] ? api_key : NULL,
                                      body, &opts, &status, np_sse_chunk_cb, &st);
    free(body);
    /* Truncation must be reported before the generic failure branch: the
     * partial text is valid, and calling it "cancelled" hides the real cause. */
    if (st.truncated && err && err_cap)
        snprintf(err, err_cap, "输出超过 %zu 字节，已保留前部内容", out_cap);
    else if (!ok && err && err_cap && !err[0]) {
        if (cancel && *cancel) snprintf(err, err_cap, "已停止");
        else snprintf(err, err_cap, "LLM 服务请求失败（HTTP %d）", status);
    }
    if (!out[0] && ok && err && err_cap && !err[0])
        snprintf(err, err_cap, "LLM 服务未返回文本内容（HTTP %d）", status);
    /* A truncated stream still carries valid text: report it as a partial
     * success so the UI offers the received content for review, with the
     * truncation reason already written into err. A genuine cancel returns
     * ok == false from the HTTP layer and is still reported as a failure. */
    return ok && out[0];
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
    /* Collect "id":"..." occurrences inside the "data" array only. Some
     * gateways add a top-level "id" (request id) or metadata ids; scanning
     * the whole response would offer fake model choices. */
    int count = 0;
    size_t o = 0;
    const char *p = strstr(resp, "\"data\"");
    if (!p) p = resp;
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
