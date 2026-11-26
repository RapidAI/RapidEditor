/* np_searchprofile.c — X9 SearchProfile（见 np_searchprofile.h）。
 * 纯本地、UI-free、无网络依赖：传输与密钥均注入。 */
#include "np_searchprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct np_sp_store {
    np_sp_profile_t items[NP_SP_MAX_PROFILES];
    size_t n;
};

/* ---------------- 小工具 ---------------- */
static void scopy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = 0;
    return s;
}
static bool ci_eq(const char *a, const char *b, size_t n) {
    if (strlen(b) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
static const char *skip_space(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p;
}

const char *np_sp_status_name(np_sp_status_t s) {
    switch (s) {
        case NP_SP_OK: return "ok";
        case NP_SP_ERR_ARG: return "bad-argument";
        case NP_SP_ERR_BAD_URL: return "bad-url";
        case NP_SP_ERR_SECRET_LEAK: return "secret-leak";
        case NP_SP_ERR_DOMAIN_DENIED: return "domain-denied";
        case NP_SP_ERR_PARSE: return "parse-error";
        case NP_SP_ERR_HTTP: return "http-error";
        case NP_SP_ERR_LIMIT: return "limit";
        case NP_SP_ERR_NO_PROFILE: return "no-profile";
        case NP_SP_ERR_DISABLED: return "disabled";
        case NP_SP_ERR_CYCLE: return "fallback-cycle";
        default: return "unknown";
    }
}
const char *np_sp_backend_name(np_sp_backend_t b) {
    return b == NP_SP_LOCAL_FTS ? "local_fts" : "http_json";
}

/* ---------------- 密钥形态检测 ----------------
 * 只识别常见明文形态；引用名（keychain:/env:/vault:）不会命中。 */
bool np_sp_looks_like_secret(const char *s, const char **reason) {
    if (!s || !*s) return false;
    static const char *PREFIXES[] = {
        "sk-", "sk_live_", "sk_test_", "ghp_", "gho_", "ghu_", "ghs_",
        "github_pat_", "AKIA", "ASIA", "xoxb-", "xoxp-", "AIza",
        "SG.", "-----BEGIN", "eyJ"
    };
    for (size_t i = 0; i < sizeof PREFIXES / sizeof PREFIXES[0]; ++i) {
        if (strncmp(s, PREFIXES[i], strlen(PREFIXES[i])) == 0) {
            if (reason) *reason = "疑似明文 API Key/令牌前缀";
            return true;
        }
    }
    /* "Bearer eyJ..." / "token abc...": 内联授权头 */
    const char *p = s;
    while (*p) {
        if ((ci_eq(p, "Bearer ", 7)) ||
            (ci_eq(p, "token ", 6)) ||
            (ci_eq(p, "api_key=", 8)) ||
            (ci_eq(p, "apikey=", 7))) {
            const char *v = skip_space(p + (ci_eq(p,"Bearer ",7)?7:
                                             ci_eq(p,"token ",6)?6:
                                             ci_eq(p,"api_key=",8)?8:7),
                                       s + strlen(s));
            if (*v && strlen(v) >= 16) {
                if (reason) *reason = "配置中出现内联授权头/密钥赋值";
                return true;
            }
        }
        ++p;
    }
    /* 长无结构高熵串（>=32 连续 base64url/hex），排除 URL/引用名 */
    if (!strstr(s, "://") && !strstr(s, ":") && strlen(s) >= 32) {
        size_t run = 0;
        for (const char *q = s; *q; ++q) {
            unsigned char c = (unsigned char)*q;
            if (isalnum(c) || c == '_' || c == '-' || c == '+' || c == '/' || c == '=') {
                if (++run >= 32) {
                    if (reason) *reason = "疑似长随机令牌（无结构高熵串）";
                    return true;
                }
            } else run = 0;
        }
    }
    return false;
}

/* ---------------- 校验 ---------------- */
np_sp_status_t np_sp_validate(const np_sp_profile_t *p, char *why, size_t why_cap) {
    if (!p) return NP_SP_ERR_ARG;
    if (!p->name[0]) {
        scopy(why, why_cap, "缺少 profile 名称");
        return NP_SP_ERR_ARG;
    }
    if (p->backend != NP_SP_HTTP_JSON && p->backend != NP_SP_LOCAL_FTS) {
        scopy(why, why_cap, "未知后端类型");
        return NP_SP_ERR_ARG;
    }
    if (p->max_results < 1 || p->max_results > NP_SP_MAX_RESULTS) {
        scopy(why, why_cap, "max_results 必须在 1..50");
        return NP_SP_ERR_LIMIT;
    }
    if (p->timeout_sec < 1 || p->timeout_sec > 120) {
        scopy(why, why_cap, "timeout_sec 必须在 1..120");
        return NP_SP_ERR_LIMIT;
    }
    /* 所有可能承载密钥的字段都做形态检测；auth_ref 本身必须是引用而非明文。 */
    const char *fields[] = { p->auth_ref, p->endpoint, p->query_param,
                             p->result_path, p->field_title, p->field_url,
                             p->field_snippet, p->domain_allow, p->domain_block };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        const char *reason = NULL;
        if (np_sp_looks_like_secret(fields[i], &reason)) {
            scopy(why, why_cap, reason);
            return NP_SP_ERR_SECRET_LEAK;
        }
    }
    if (p->backend == NP_SP_HTTP_JSON) {
        const char *e = p->endpoint;
        if (!(strncmp(e, "http://", 7) == 0 || strncmp(e, "https://", 8) == 0)) {
            scopy(why, why_cap, "HTTP 后端端点必须以 http:// 或 https:// 开头");
            return NP_SP_ERR_BAD_URL;
        }
        if (p->method[0] && strcmp(p->method, "POST") && strcmp(p->method, "GET")) {
            scopy(why, why_cap, "method 只能是 GET 或 POST");
            return NP_SP_ERR_ARG;
        }
    }
    return NP_SP_OK;
}

/* ---------------- store ---------------- */
np_sp_store_t *np_sp_store_create(void) {
    return (np_sp_store_t *)calloc(1, sizeof(np_sp_store_t));
}
void np_sp_store_destroy(np_sp_store_t *st) { free(st); }
size_t np_sp_count(const np_sp_store_t *st) { return st ? st->n : 0; }
const np_sp_profile_t *np_sp_at(const np_sp_store_t *st, size_t i) {
    return (st && i < st->n) ? &st->items[i] : NULL;
}
int np_sp_index_of(const np_sp_store_t *st, const char *name) {
    if (!st || !name) return -1;
    for (size_t i = 0; i < st->n; ++i)
        if (strcmp(st->items[i].name, name) == 0) return (int)i;
    return -1;
}
const np_sp_profile_t *np_sp_get(const np_sp_store_t *st, const char *name) {
    int i = np_sp_index_of(st, name);
    return i < 0 ? NULL : &st->items[i];
}
np_sp_status_t np_sp_add(np_sp_store_t *st, const np_sp_profile_t *p) {
    if (!st || !p) return NP_SP_ERR_ARG;
    char why[128];
    np_sp_status_t s = np_sp_validate(p, why, sizeof why);
    if (s != NP_SP_OK) return s;
    int at = np_sp_index_of(st, p->name);
    if (at < 0) {
        if (st->n >= NP_SP_MAX_PROFILES) return NP_SP_ERR_LIMIT;
        at = (int)st->n++;
    }
    st->items[at] = *p;   /* 全部定长数组，结构体赋值即深拷贝 */
    return NP_SP_OK;
}
bool np_sp_remove(np_sp_store_t *st, const char *name) {
    int at = np_sp_index_of(st, name);
    if (at < 0 || !st) return false;
    for (size_t i = (size_t)at; i + 1 < st->n; ++i) st->items[i] = st->items[i+1];
    st->n--;
    return true;
}
bool np_sp_set_enabled(np_sp_store_t *st, const char *name, bool enabled) {
    int at = np_sp_index_of(st, name);
    if (at < 0 || !st) return false;
    st->items[at].enabled = enabled;
    return true;
}

/* ---------------- 序列化（INI） ---------------- */
static const char *kv_line(FILE *f, char *buf, size_t cap) {
    if (!fgets(buf, (int)cap, f)) return NULL;
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}
bool np_sp_store_save(const np_sp_store_t *st, const char *path) {
    if (!st || !path) return false;
    char why[128];
    for (size_t i = 0; i < st->n; ++i)
        if (np_sp_validate(&st->items[i], why, sizeof why) != NP_SP_OK) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    for (size_t i = 0; i < st->n && ok; ++i) {
        const np_sp_profile_t *p = &st->items[i];
        if (fprintf(f, "[profile:%s]\n", p->name) < 0) { ok = false; break; }
        struct { const char *k; const char *v; } s[] = {
            {"backend", p->backend == NP_SP_LOCAL_FTS ? "local_fts" : "http_json"},
            {"enabled", p->enabled ? "true" : "false"},
            {"endpoint", p->endpoint}, {"method", p->method},
            {"auth_ref", p->auth_ref}, {"query_param", p->query_param},
            {"result_path", p->result_path}, {"field_title", p->field_title},
            {"field_url", p->field_url}, {"field_snippet", p->field_snippet},
            {"domain_allow", p->domain_allow}, {"domain_block", p->domain_block},
            {"fallback", p->fallback}
        };
        for (size_t j = 0; j < sizeof s / sizeof s[0]; ++j)
            if (fprintf(f, "%s=%s\n", s[j].k, s[j].v) < 0) { ok = false; break; }
        if (fprintf(f, "max_results=%d\ntimeout_sec=%d\npriority=%d\n\n",
                    p->max_results, p->timeout_sec, p->priority) < 0) ok = false;
    }
    fclose(f);
    return ok;
}
size_t np_sp_store_load(np_sp_store_t *st, const char *path) {
    if (!st || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t before = st->n;
    np_sp_profile_t cur; memset(&cur, 0, sizeof cur);
    bool have = false;
    char line[1024];
    while (kv_line(f, line, sizeof line)) {
        char *t = trim(line);
        if (!*t) continue;
        if (t[0] == '#') continue;
        if (t[0] == '[') {
            /* 段落结束：提交上一段 */
            if (have) { np_sp_add(st, &cur); memset(&cur, 0, sizeof cur); }
            char *close = strchr(t, ']');
            if (close) *close = 0;
            if (strncmp(t + 1, "profile:", 8) == 0) {
                scopy(cur.name, sizeof cur.name, t + 9);
                cur.backend = NP_SP_HTTP_JSON;
                cur.enabled = true;
                scopy(cur.method, sizeof cur.method, "GET");
                scopy(cur.query_param, sizeof cur.query_param, "q");
                scopy(cur.field_title, sizeof cur.field_title, "title");
                scopy(cur.field_url, sizeof cur.field_url, "url");
                scopy(cur.field_snippet, sizeof cur.field_snippet, "snippet");
                cur.max_results = 10; cur.timeout_sec = 15;
                have = true;
            } else have = false;
            continue;
        }
        if (!have) continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(t), *v = trim(eq + 1);
        if (!strcmp(k, "backend")) cur.backend = !strcmp(v,"local_fts") ? NP_SP_LOCAL_FTS : NP_SP_HTTP_JSON;
        else if (!strcmp(k, "enabled")) cur.enabled = !strcmp(v, "false") ? false : true;
        else if (!strcmp(k, "endpoint")) scopy(cur.endpoint, sizeof cur.endpoint, v);
        else if (!strcmp(k, "method")) scopy(cur.method, sizeof cur.method, v);
        else if (!strcmp(k, "auth_ref")) scopy(cur.auth_ref, sizeof cur.auth_ref, v);
        else if (!strcmp(k, "query_param")) scopy(cur.query_param, sizeof cur.query_param, v);
        else if (!strcmp(k, "result_path")) scopy(cur.result_path, sizeof cur.result_path, v);
        else if (!strcmp(k, "field_title")) scopy(cur.field_title, sizeof cur.field_title, v);
        else if (!strcmp(k, "field_url")) scopy(cur.field_url, sizeof cur.field_url, v);
        else if (!strcmp(k, "field_snippet")) scopy(cur.field_snippet, sizeof cur.field_snippet, v);
        else if (!strcmp(k, "domain_allow")) scopy(cur.domain_allow, sizeof cur.domain_allow, v);
        else if (!strcmp(k, "domain_block")) scopy(cur.domain_block, sizeof cur.domain_block, v);
        else if (!strcmp(k, "fallback")) scopy(cur.fallback, sizeof cur.fallback, v);
        else if (!strcmp(k, "max_results")) cur.max_results = atoi(v);
        else if (!strcmp(k, "timeout_sec")) cur.timeout_sec = atoi(v);
        else if (!strcmp(k, "priority")) cur.priority = atoi(v);
    }
    if (have) np_sp_add(st, &cur);
    fclose(f);
    return st->n - before;
}

/* ---------------- 域名门禁 ---------------- */
static bool host_match(const char *host, const char *rule) {
    rule = trim((char *)rule);
    if (!*rule) return false;
    if (strncmp(rule, "*.", 2) == 0) {
        const char *domain = rule + 2;             /* "wiki.example.org" */
        size_t hl = strlen(host), dl = strlen(domain);
        if (hl <= dl) return false;
        /* 必须有一个子域标签：host 以 "."+domain 结尾 */
        const char *tail = host + (hl - dl);
        return ci_eq(tail, domain, dl) && tail > host && tail[-1] == '.';
    }
    return ci_eq(host, rule, strlen(rule));
}
static const char *url_host(const char *url, char *buf, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    const char *e = p;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') ++e;
    size_t n = (size_t)(e - p);
    if (n == 0 || n + 1 > cap) return NULL;
    memcpy(buf, p, n); buf[n] = 0;
    return buf;
}
/* 逗号分隔列表中是否命中（就地拷贝避免改原串）。 */
static bool list_has(const char *list, const char *host) {
    char buf[NP_SP_LIST_CAP];
    scopy(buf, sizeof buf, list);
    char *p = buf;
    while (*p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *rule = trim(p);
        if (host_match(host, rule)) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}
bool np_sp_domain_allowed(const np_sp_profile_t *p, const char *url) {
    if (!p || !url) return false;
    char host[NP_SP_URL_CAP];
    if (!url_host(url, host, sizeof host)) return false;
    if (list_has(p->domain_block, host)) return false;         /* block 优先 */
    if (!p->domain_allow[0]) return false;                     /* fail-closed */
    return list_has(p->domain_allow, host);
}

/* ---------------- PII 脱敏 ---------------- */
void np_sp_redact(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!in) { out[0] = 0; return; }
    size_t o = 0;
    size_t n = strlen(in);
    size_t i = 0;
    while (i < n && o + 1 < cap) {
        /* 邮箱：从 local 起始处正向识别整段（避免已写出的 local 残留） */
        bool local_char = isalnum((unsigned char)in[i]) || in[i] == '.' ||
                          in[i] == '_' || in[i] == '+' || in[i] == '-';
        bool prev_local = i > 0 && (isalnum((unsigned char)in[i-1]) || in[i-1] == '.' ||
                          in[i-1] == '_' || in[i-1] == '+' || in[i-1] == '-');
        if (local_char && !prev_local) {
            size_t at = i;
            while (at < n && (isalnum((unsigned char)in[at]) ||
                   in[at] == '.' || in[at] == '_' || in[at] == '+' || in[at] == '-')) ++at;
            if (at < n && in[at] == '@') {
                size_t e = at + 1;
                bool has_dot = false;
                while (e < n && (isalnum((unsigned char)in[e]) ||
                       in[e] == '.' || in[e] == '-' || in[e] == '_')) {
                    if (in[e] == '.') has_dot = true;
                    ++e;
                }
                if (e > at + 1 && has_dot && o + 8 < cap) {
                    const char tag[] = "[EMAIL]";
                    memcpy(out + o, tag, sizeof tag - 1); o += sizeof tag - 1;
                    i = e; continue;
                }
            }
        }
        /* 数字串：18位身份证 / 11位手机号 / >=12 长数字 */
        if (isdigit((unsigned char)in[i])) {
            size_t e = i;
            while (e < n && isdigit((unsigned char)in[e])) ++e;
            size_t L = e - i;
            if ((L == 17 && e < n && (in[e] == 'X' || in[e] == 'x')) || L == 18) {
                if (o + 4 < cap) { memcpy(out + o, "[ID]", 4); o += 4; }
                i = (L == 17) ? e + 1 : e; continue;
            }
            if (L == 11 && in[i] == '1' && in[i+1] >= '3' && in[i+1] <= '9') {
                if (o + 7 < cap) { memcpy(out + o, "[PHONE]", 7); o += 7; }
                i = e; continue;
            }
            if (L >= 12) {
                if (o + 5 < cap) { memcpy(out + o, "[NUM]", 5); o += 5; }
                i = e; continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

/* ---------------- 最小 JSON 解析器 ----------------
 * 只做定位/取串，不构建树；深度与 token 数双重上限。 */
typedef struct {
    const char *s;
    size_t len, pos, tokens, depth;
} jp_t;
static void jws(jp_t *j) {
    while (j->pos < j->len && isspace((unsigned char)j->s[j->pos])) ++j->pos;
}
/* 解析一个值，返回 [start,end)；kind: 0=str 1=obj 2=arr 3=scalar -1=err */
static int jvalue(jp_t *j, size_t *st, size_t *en) {
    jws(j);
    if (j->pos >= j->len || ++j->tokens > NP_SP_JSON_MAX_TOKENS) return -1;
    *st = j->pos;
    char c = j->s[j->pos];
    if (c == '"') {
        ++j->pos;
        while (j->pos < j->len) {
            char d = j->s[j->pos++];
            if (d == '\\') { if (j->pos < j->len) ++j->pos; }
            else if (d == '"') { *en = j->pos; return 0; }
        }
        return -1;
    }
    if (c == '{' || c == '[') {
        if (++j->depth > NP_SP_JSON_MAX_DEPTH) return -1;
        char close = c == '{' ? '}' : ']';
        ++j->pos;
        while (j->pos < j->len) {
            jws(j);
            if (j->pos < j->len && j->s[j->pos] == close) {
                ++j->pos; --j->depth; *en = j->pos;
                return c == '{' ? 1 : 2;
            }
            if (j->pos < j->len && (j->s[j->pos] == '"' || j->s[j->pos] == '{' ||
                j->s[j->pos] == '[')) {
                size_t a, b;
                if (jvalue(j, &a, &b) < 0) return -1;
            } else ++j->pos;
        }
        return -1;
    }
    while (j->pos < j->len && j->s[j->pos] != ',' && j->s[j->pos] != '}' &&
           j->s[j->pos] != ']' && !isspace((unsigned char)j->s[j->pos])) ++j->pos;
    *en = j->pos;
    return 3;
}
/* 在对象节点里找 key 的值节点 */
static bool jmember(jp_t *j, size_t os, size_t oe, const char *key,
                    size_t *vs, size_t *ve, int *kind) {
    size_t p = os + 1;
    while (p < oe) {
        p = (size_t)(skip_space(j->s + p, j->s + oe) - j->s);
        if (p >= oe || j->s[p] != '"') break;
        size_t ks, ke;
        jp_t sub = { j->s, oe, p, j->tokens, j->depth };
        if (jvalue(&sub, &ks, &ke) < 0) return false;
        j->tokens = sub.tokens;
        p = (size_t)(skip_space(j->s + ke, j->s + oe) - j->s);
        if (p >= oe || j->s[p] != ':') { p = ke; continue; }
        ++p;
        jp_t vsub = { j->s, oe, p, j->tokens, j->depth };
        int k = jvalue(&vsub, vs, ve);
        j->tokens = vsub.tokens; j->depth = vsub.depth;
        if (k < 0) return false;
        /* 键名匹配（解码简化：仅比较裸字节） */
        if (ke >= ks + 2 && strlen(key) == ke - ks - 2 &&
            memcmp(j->s + ks + 1, key, strlen(key)) == 0) {
            if (kind) *kind = k;
            return true;
        }
        p = (size_t)(skip_space(j->s + *ve, j->s + oe) - j->s);
        if (p < oe && j->s[p] == ',') ++p;
    }
    return false;
}
bool np_sp_json_slice(const char *json, size_t json_len, const char *path,
                      size_t *out_start, size_t *out_len) {
    if (!json || !path) return false;
    jp_t j = { json, json_len, 0, 0, 0 };
    size_t cs, ce;
    int kind = jvalue(&j, &cs, &ce);
    if (kind < 0) return false;
    if (!path[0]) { *out_start = cs; *out_len = ce - cs; return true; }
    char seg[NP_SP_PATH_CAP];
    scopy(seg, sizeof seg, path);
    char *ctx = seg;
    while (ctx && *ctx) {
        char *dot = strchr(ctx, '.');
        if (dot) *dot = 0;
        if (kind == 1) {
            int nk;
            if (!jmember(&j, cs, ce, ctx, &cs, &ce, &nk)) return false;
            kind = nk;
        } else if (kind == 2) {
            char *endp = NULL;
            long idx = strtol(ctx, &endp, 10);
            if (endp == ctx || idx < 0) return false;
            long cur = 0;
            size_t p = cs + 1;
            while (p < ce) {
                p = (size_t)(skip_space(json + p, json + ce) - json);
                if (p >= ce || json[p] == ']') break;
                size_t vs, ve; jp_t sub = { json, ce, p, j.tokens, j.depth };
                int sk = jvalue(&sub, &vs, &ve);
                if (sk < 0) return false;
                j.tokens = sub.tokens; j.depth = sub.depth;
                if (cur == idx) { cs = vs; ce = ve; kind = sk; break; }
                ++cur; p = (size_t)(skip_space(json + ve, json + ce) - json);
                if (p < ce && json[p] == ',') ++p;
            }
            if (cur != idx) return false;
        } else return false;
        ctx = dot ? dot + 1 : NULL;
    }
    *out_start = cs; *out_len = ce - cs;
    return true;
}
/* 从 [start,start+len) 的节点（对象或字符串）中取 key 的字符串值 */
bool np_sp_json_string(const char *json, size_t start, size_t len,
                       const char *key, char *out, size_t cap) {
    if (!json || !out || cap == 0) return false;
    out[0] = 0;
    size_t end = start + len;
    if (len == 0 || start >= end) return false;
    if (json[start] == '{') {
        jp_t j = { json, end, 0, 0, 0 };
        size_t vs, ve; int kind;
        if (!jmember(&j, start, end, key, &vs, &ve, &kind) || kind != 0) return false;
        start = vs; end = ve;
    }
    if (json[start] != '"' || end <= start + 1) return false;
    size_t o = 0;
    for (size_t i = start + 1; i + 1 < end && o + 1 < cap; ++i) {
        char c = json[i];
        if (c == '\\' && i + 2 < end) {
            char d = json[++i];
            if (d == 'n') c = '\n'; else if (d == 'r') c = '\r';
            else if (d == 't') c = '\t'; else if (d == 'u') {
                /* \uXXXX 简化为非 ASCII 占位，避免宽字符依赖 */
                if (i + 4 < end) { c = '?'; i += 4; } else c = '?';
            } else c = d;
        } else if (c == '"') break;
        out[o++] = c;
    }
    out[o] = 0;
    return true;
}

/* ---------------- URL 编码 ---------------- */
static void url_encode(const char *s, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 3 < cap; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++]='%'; out[o++]=HEX[c>>4]; out[o++]=HEX[c&15]; }
    }
    out[o] = 0;
}

/* ---------------- HTTP 召回 ---------------- */
static void hit_fill(np_sp_hit_t *h, const np_sp_profile_t *p,
                     const char *title, const char *url, const char *snip) {
    np_sp_redact(title, h->title, sizeof h->title);
    np_sp_redact(url, h->url, sizeof h->url);
    np_sp_redact(snip, h->snippet, sizeof h->snippet);
    scopy(h->source, sizeof h->source, p->name);
}
np_sp_status_t np_sp_recall_http(const np_sp_profile_t *p, const char *query,
                                 np_sp_fetch_fn fetch, np_sp_secret_fn resolve,
                                 void *user, np_sp_hit_t *out, int out_cap,
                                 int *out_total) {
    if (!p || !query || !fetch || !out || out_cap <= 0) return NP_SP_ERR_ARG;
    if (p->backend != NP_SP_HTTP_JSON) return NP_SP_ERR_ARG;
    if (!p->enabled) return NP_SP_ERR_DISABLED;
    if (!np_sp_domain_allowed(p, p->endpoint)) return NP_SP_ERR_DOMAIN_DENIED;
    char enc[512]; url_encode(query, enc, sizeof enc);
    char url[NP_SP_URL_CAP + 8 + NP_SP_FIELD_CAP + 512];
    const char *qp = p->query_param[0] ? p->query_param : "q";
    /* 手写有界拼接，避免 snprintf 对最坏路径的 -Wformat-truncation 误报 */
    size_t u = 0;
    for (const char *q = p->endpoint; *q && u + 1 < sizeof url; ++q) url[u++] = *q;
    if (u + 1 < sizeof url) url[u++] = strchr(p->endpoint, '?') ? '&' : '?';
    for (const char *q = qp; *q && u + 1 < sizeof url; ++q) url[u++] = *q;
    if (u + 1 < sizeof url) url[u++] = '=';
    for (const char *q = enc; *q && u + 1 < sizeof url; ++q) url[u++] = *q;
    url[u] = 0;
    if (!np_sp_domain_allowed(p, url)) return NP_SP_ERR_DOMAIN_DENIED;
    const char *secret = resolve ? resolve(p->auth_ref, user) : NULL;
    int status = 0;
    char *resp = fetch(url, p->method[0] ? p->method : "GET", secret,
                       p->timeout_sec, user, &status);
    if (!resp) return NP_SP_ERR_HTTP;
    size_t rlen = strlen(resp);
    if (rlen > NP_SP_RESP_CAP) { free(resp); return NP_SP_ERR_LIMIT; }
    int kept = 0, total = 0;
    size_t as, al;
    if (np_sp_json_slice(resp, rlen, p->result_path, &as, &al)) {
        /* 结果节点必须是数组；逐个对象解析字段 */
        if (as < rlen && resp[as] == '[') {
            size_t pos = as + 1;
            while (pos < as + al) {
                pos = (size_t)(skip_space(resp + pos, resp + as + al) - resp);
                if (pos >= as + al || resp[pos] == ']') break;
                size_t os, oe; jp_t sub = { resp, as + al, pos, 0, 0 };
                int kind = jvalue(&sub, &os, &oe);
                if (kind < 0) break;
                pos = oe;
                ++total;
                if (kind == 1 && kept < out_cap) {
                    char t[NP_SP_TITLE_CAP], u[NP_SP_URL_CAP], s[NP_SP_SNIPPET_CAP];
                    t[0]=u[0]=s[0]=0;
                    np_sp_json_string(resp, os, oe - os, p->field_title, t, sizeof t);
                    np_sp_json_string(resp, os, oe - os, p->field_url, u, sizeof u);
                    np_sp_json_string(resp, os, oe - os, p->field_snippet, s, sizeof s);
                    hit_fill(&out[kept++], p, t, u, s);
                }
                pos = (size_t)(skip_space(resp + pos, resp + as + al) - resp);
                if (pos < as + al && resp[pos] == ',') ++pos;
            }
        }
    } else { free(resp); return NP_SP_ERR_PARSE; }
    if (total > p->max_results) total = p->max_results;
    if (out_total) *out_total = total;
    free(resp);
    return kept ? NP_SP_OK : NP_SP_ERR_PARSE;
}

/* ---------------- 本地召回 ---------------- */
np_sp_status_t np_sp_recall_local(const np_sp_profile_t *p, const char *query,
                                  np_sp_local_fn local, void *user,
                                  np_sp_hit_t *out, int out_cap, int *out_total) {
    if (!p || !query || !local || !out || out_cap <= 0) return NP_SP_ERR_ARG;
    if (p->backend != NP_SP_LOCAL_FTS) return NP_SP_ERR_ARG;
    if (!p->enabled) return NP_SP_ERR_DISABLED;
    char paths[NP_SP_MAX_RESULTS][260];
    memset(paths, 0, sizeof paths);
    int cap = out_cap < p->max_results ? out_cap : p->max_results;
    if (cap > NP_SP_MAX_RESULTS) cap = NP_SP_MAX_RESULTS;
    size_t n = local(query, paths, (size_t)cap, user);
    if (n > (size_t)cap) n = (size_t)cap;
    for (size_t i = 0; i < n; ++i) hit_fill(&out[i], p, paths[i], paths[i], "");
    if (out_total) *out_total = (int)n;
    return n ? NP_SP_OK : NP_SP_ERR_PARSE;
}

/* ---------------- fallback 链 ---------------- */
np_sp_status_t np_sp_recall_chain(np_sp_store_t *st, const char *start_name,
                                  const char *query,
                                  np_sp_fetch_fn fetch, np_sp_secret_fn resolve,
                                  np_sp_local_fn local, void *user,
                                  np_sp_hit_t *out, int out_cap, int *out_total,
                                  char *used_name, size_t used_cap) {
    if (!st || !query || !out || out_cap <= 0) return NP_SP_ERR_ARG;
    if (used_name && used_cap) used_name[0] = 0;
    int cur = start_name && start_name[0] ? np_sp_index_of(st, start_name) : -1;
    if (cur < 0) {
        /* 首个启用项 */
        for (size_t i = 0; i < st->n; ++i)
            if (st->items[i].enabled) { cur = (int)i; break; }
    }
    if (cur < 0) return NP_SP_ERR_NO_PROFILE;
    int visited[NP_SP_MAX_PROFILES]; size_t nv = 0;
    while (cur >= 0 && cur < (int)st->n && nv < NP_SP_MAX_PROFILES) {
        for (size_t i = 0; i < nv; ++i) if (visited[i] == cur) return NP_SP_ERR_CYCLE;
        visited[nv++] = cur;
        const np_sp_profile_t *p = &st->items[cur];
        int got = 0;
        np_sp_status_t s = p->backend == NP_SP_LOCAL_FTS
            ? np_sp_recall_local(p, query, local, user, out, out_cap, &got)
            : np_sp_recall_http(p, query, fetch, resolve, user, out, out_cap, &got);
        if (s == NP_SP_OK) {
            if (out_total) *out_total = got;
            if (used_name && used_cap) scopy(used_name, used_cap, p->name);
            return NP_SP_OK;
        }
        if (!p->fallback[0]) break;
        int next = np_sp_index_of(st, p->fallback);
        if (next < 0) break;
        cur = next;
    }
    return NP_SP_ERR_NO_PROFILE;
}
