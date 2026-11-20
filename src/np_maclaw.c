#include "np_maclaw.h"
#include "np_net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void np_maclaw_normalize_hub(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in) return;
    const char *p = in;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = strlen(p);
    while (n > 0 && isspace((unsigned char)p[n - 1])) n--;
    while (n > 0 && p[n - 1] == '/') n--;
    if (n == 0 || n >= cap) return;
    if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
        if (n >= cap) return;
        memcpy(out, p, n); out[n] = 0;
    } else {
        if (n + 8 >= cap) return;
        snprintf(out, cap, "https://%.*s", (int)n, p);
    }
}

/* JSON string unescape into out. Handles \" \\ \/ \b \f \n \r \t and \uXXXX
 * (UTF-8 encoded). */
static void json_unescape(const char *src, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        if (src[i] != '\\' || i + 1 >= len) { out[o++] = src[i]; continue; }
        i++;
        switch (src[i]) {
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'u': {
                if (i + 4 >= len) break;
                unsigned cp = 0;
                for (int k = 1; k <= 4; k++) {
                    char c = src[i + k];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                }
                i += 4;
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
            default: out[o++] = src[i]; break;
        }
    }
    out[o] = 0;
}

bool np_json_get_string(const char *json, const char *key, char *out, size_t cap) {
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
    json_unescape(start, (size_t)(p - start), out, cap);
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

bool np_maclaw_create_session(const np_maclaw_cfg *cfg, np_maclaw_session *s) {
    if (!cfg || !s || !cfg->hub_url[0]) return false;
    s->err[0] = 0;
    char cid[256], cname[256];
    json_escape(cfg->client_id, cid, sizeof cid);
    json_escape(cfg->client_name, cname, sizeof cname);
    char body[1024];
    snprintf(body, sizeof body,
             "{\"client_id\":\"%s\",\"name\":\"%s\"}", cid, cname);
    char url[1024];
    snprintf(url, sizeof url, "%s/api/mobile/auth/third-party-qr-sessions", cfg->hub_url);

    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.err = s->err; opts.err_cap = sizeof s->err;
    opts.timeout_sec = cfg->timeout_sec > 0 ? cfg->timeout_sec : 30;

    int status = 0;
    char *resp = np_net_post_json(url, NULL, body, &opts, &status);
    if (!resp) { if (!s->err[0]) snprintf(s->err, sizeof s->err, "HTTP %d", status); return false; }
    bool ok = false;
    if (status >= 200 && status < 300 &&
        np_json_get_string(resp, "session_id", s->session_id, sizeof s->session_id) &&
        np_json_get_string(resp, "poll_token", s->poll_token, sizeof s->poll_token) &&
        np_json_get_string(resp, "qr_payload", s->qr_payload, sizeof s->qr_payload)) {
        np_json_get_string(resp, "expires_at", s->expires_at, sizeof s->expires_at);
        ok = true;
    } else {
        snprintf(s->err, sizeof s->err, "建会话失败 HTTP %d", status);
    }
    free(resp);
    return ok;
}

const char *np_maclaw_poll(const np_maclaw_cfg *cfg, const np_maclaw_session *s,
                           char *token, size_t token_cap,
                           char *email, size_t email_cap,
                           char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (token && token_cap) token[0] = 0;
    if (email && email_cap) email[0] = 0;
    if (!cfg || !s || !cfg->hub_url[0] || !s->session_id[0]) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return NULL;
    }
    char sid[256], pt[256];
    json_escape(s->session_id, sid, sizeof sid);
    json_escape(s->poll_token, pt, sizeof pt);
    char body[1024];
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"poll_token\":\"%s\"}", sid, pt);
    char url[1024];
    snprintf(url, sizeof url, "%s/api/mobile/auth/third-party-qr-sessions/token", cfg->hub_url);

    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    if (err) { opts.err = err; opts.err_cap = err_cap; }
    opts.timeout_sec = cfg->timeout_sec > 0 ? cfg->timeout_sec : 30;

    int status = 0;
    char *resp = np_net_post_json(url, NULL, body, &opts, &status);
    if (!resp) return NULL;
    char st[64] = {0};
    np_json_get_string(resp, "status", st, sizeof st);

    if (!strcmp(st, "authorized")) {
        if (token && token_cap)
            np_json_get_string(resp, "access_token", token, token_cap);
        if (email && email_cap)
            np_json_get_string(resp, "email", email, email_cap);
        free(resp);
        return "authorized";
    }
    free(resp);
    if (!strcmp(st, "pending")) return "pending";
    if (!strcmp(st, "delivered")) return "delivered";
    if (!strcmp(st, "expired")) return "expired";
    if (err && err_cap) snprintf(err, err_cap, "未知状态: %s (HTTP %d)", st, status);
    return NULL;
}

bool np_maclaw_llm_chat(const char *hub_url, const char *token,
                        const char *prompt, char *out, size_t out_cap,
                        char *err, size_t err_cap) {
    if (out && out_cap) out[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!hub_url || !token || !prompt || !out || !out_cap) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return false;
    }

    char hub[600];
    np_maclaw_normalize_hub(hub_url, hub, sizeof hub);
    if (!hub[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Hub 地址无效");
        return false;
    }
    char url[1200];
    snprintf(url, sizeof url, "%s/api/llm/v1/chat/completions", hub);

    size_t esc_cap = strlen(prompt) * 6 + 64;
    char *escaped = (char *)malloc(esc_cap);
    if (!escaped) {
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    json_escape(prompt, escaped, esc_cap);
    size_t body_cap = strlen(escaped) + 512;
    char *body = (char *)malloc(body_cap);
    if (!body) {
        free(escaped);
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return false;
    }
    snprintf(body, body_cap,
             "{\"model\":\"auto\",\"stream\":false,"
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             escaped);
    free(escaped);

    np_net_opts opts;
    memset(&opts, 0, sizeof opts);
    if (err) { opts.err = err; opts.err_cap = err_cap; }
    opts.timeout_sec = 120;
    int status = 0;
    char *resp = np_net_post_json(url, token, body, &opts, &status);
    free(body);
    if (!resp) {
        if (err && err_cap && !err[0])
            snprintf(err, err_cap, "maclaw 官方服务请求失败（HTTP %d）", status);
        return false;
    }
    bool ok = np_json_get_string(resp, "content", out, out_cap) && out[0];
    if (!ok && err && err_cap)
        snprintf(err, err_cap, "maclaw 官方服务未返回文本内容（HTTP %d）", status);
    free(resp);
    return ok;
}
