/* np_mcp.c — S1: X6 MCP 工具服务器（见 np_mcp.h）。
 * 纯本地、UI-free、零网络依赖：stdio/HTTP 传输与密钥解析全部注入。 */
#include "np_mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define NP_MCP_JSON_MAX_DEPTH  24
#define NP_MCP_JSON_MAX_TOKENS 4096
#define NP_MCP_PATH_CAP        256
#define NP_MCP_REQ_CAP         (128 * 1024)

/* 每个 server 附加运行时发现到的工具列表（不持久化，启动时重新发现）。 */
typedef struct {
    np_mcp_server_t srv;
    np_mcp_tool_t   tools[NP_MCP_MAX_TOOLS];
    size_t          tool_count;
} np_mcp_item_t;

struct np_mcp_store {
    np_mcp_item_t items[NP_MCP_MAX_SERVERS];
    size_t        n;
};

/* ---------------- 小工具 ---------------- */
static void mcopy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}
static char *mtrim(char *s) {
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
static size_t bcat(char *buf, size_t cap, size_t at, const char *s) {
    if (at >= cap) { if (cap) buf[cap - 1] = 0; return at; }
    size_t i = at;
    while (i + 1 < cap && *s) buf[i++] = *s++;
    buf[i] = 0;
    return i;
}

const char *np_mcp_status_name(np_mcp_status_t s) {
    switch (s) {
        case NP_MCP_OK:              return "ok";
        case NP_MCP_ERR_ARG:         return "bad-argument";
        case NP_MCP_ERR_BAD_CONFIG:  return "bad-config";
        case NP_MCP_ERR_SECRET_LEAK: return "secret-leak";
        case NP_MCP_ERR_DOMAIN_DENIED: return "domain-denied";
        case NP_MCP_ERR_TRANSPORT:   return "transport-error";
        case NP_MCP_ERR_PARSE:       return "parse-error";
        case NP_MCP_ERR_HEALTH:      return "health-error";
        case NP_MCP_ERR_DISABLED:    return "disabled";
        case NP_MCP_ERR_NOT_FOUND:   return "not-found";
        case NP_MCP_ERR_LIMIT:       return "limit";
        case NP_MCP_ERR_WRITE_DENIED:return "write-denied";
        default:                     return "unknown";
    }
}
const char *np_mcp_transport_name(np_mcp_transport_t t) {
    return t == NP_MCP_STDIO ? "stdio" : "http";
}

/* ---------------- 密钥形态检测（与 X9 同口径） ---------------- */
bool np_mcp_looks_like_secret(const char *s, const char **reason) {
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
    const char *p = s;
    while (*p) {
        size_t skip = 0;
        if (ci_eq(p, "Bearer ", 7)) skip = 7;
        else if (ci_eq(p, "token ", 6)) skip = 6;
        else if (ci_eq(p, "api_key=", 8)) skip = 8;
        else if (ci_eq(p, "apikey=", 7)) skip = 7;
        if (skip) {
            const char *end = s + strlen(s);
            const char *v = skip_space(p + skip, end);
            if (*v && strlen(v) >= 16) {
                if (reason) *reason = "配置中出现内联授权头/密钥赋值";
                return true;
            }
        }
        ++p;
    }
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

/* ---------------- 配置校验 ---------------- */
np_mcp_status_t np_mcp_validate(const np_mcp_server_t *s, char *why, size_t why_cap) {
    if (!s) { mcopy(why, why_cap, "空配置"); return NP_MCP_ERR_ARG; }
    if (!s->name[0]) { mcopy(why, why_cap, "缺少服务器名称"); return NP_MCP_ERR_ARG; }
    if (s->transport != NP_MCP_STDIO && s->transport != NP_MCP_HTTP) {
        mcopy(why, why_cap, "未知传输类型"); return NP_MCP_ERR_BAD_CONFIG;
    }
    if (s->timeout_sec < 1 || s->timeout_sec > 120) {
        mcopy(why, why_cap, "timeout_sec 必须在 1..120"); return NP_MCP_ERR_LIMIT;
    }
    /* 可能承载密钥的字段统一形态检测；auth_ref 本身必须是引用而非明文。 */
    const char *fields[] = { s->name, s->command, s->endpoint, s->auth_ref,
                             s->domain_allow, s->domain_block, s->sha256 };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        const char *reason = NULL;
        if (np_mcp_looks_like_secret(fields[i], &reason)) {
            mcopy(why, why_cap, reason); return NP_MCP_ERR_SECRET_LEAK;
        }
    }
    if (s->auth_ref[0] &&
        strncmp(s->auth_ref, "keychain:", 9) &&
        strncmp(s->auth_ref, "env:", 4) &&
        strncmp(s->auth_ref, "vault:", 6)) {
        mcopy(why, why_cap, "auth_ref 必须是 keychain:/env:/vault: 引用");
        return NP_MCP_ERR_BAD_CONFIG;
    }
    if (s->transport == NP_MCP_STDIO) {
        if (!s->command[0]) { mcopy(why, why_cap, "stdio 服务器必须提供启动命令"); return NP_MCP_ERR_BAD_CONFIG; }
        /* 拒绝 shell 注入元字符：不经 shell 执行，命令模板里也不允许出现。 */
        static const char *BAD = "|&;`$()<>\n\r\\";
        if (strpbrk(s->command, BAD)) {
            mcopy(why, why_cap, "启动命令含 shell 元字符，已拒绝"); return NP_MCP_ERR_BAD_CONFIG;
        }
    } else {
        const char *e = s->endpoint;
        if (!(strncmp(e, "http://", 7) == 0 || strncmp(e, "https://", 8) == 0)) {
            mcopy(why, why_cap, "HTTP 服务器端点必须以 http:// 或 https:// 开头");
            return NP_MCP_ERR_BAD_CONFIG;
        }
    }
    return NP_MCP_OK;
}

/* ---------------- URL host + 域名门禁 ---------------- */
static bool url_host(const char *url, char *host, size_t cap) {
    if (!url || !host || cap == 0) return false;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return false;
    const char *e = p;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') ++e;
    size_t n = (size_t)(e - p);
    if (n == 0 || n >= cap) return false;
    memcpy(host, p, n); host[n] = 0;
    return true;
}
static bool list_sep(char c) { return c == ' ' || c == ',' || c == ';'; }
static bool list_has(const char *list, const char *host) {
    if (!list || !*list || !host) return false;
    const char *p = list;
    while (*p) {
        while (*p && list_sep(*p)) ++p;
        const char *tok = p;
        while (*p && !list_sep(*p)) ++p;
        size_t tl = (size_t)(p - tok);
        if (tl == 0) continue;
        if (tok[0] == '*' && tl > 1 && tok[1] == '.') {
            size_t hl = strlen(host), sl = tl - 1;            /* 去掉 '*' */
            const char *suf = tok + 1;                        /* ".example.com" */
            if (hl >= sl) {
                size_t i = 0;
                for (; i < sl; ++i)
                    if (tolower((unsigned char)host[hl - sl + i]) != tolower((unsigned char)suf[i])) break;
                if (i == sl) return true;
            }
        } else {
            /* ci_eq 要求 b 长度等于 n，这里按 token 长度精确比较 */
            if (strlen(host) == tl) {
                size_t i = 0;
                for (; i < tl; ++i)
                    if (tolower((unsigned char)tok[i]) != tolower((unsigned char)host[i])) break;
                if (i == tl) return true;
            }
        }
    }
    return false;
}
bool np_mcp_domain_allowed(const np_mcp_server_t *s, const char *url) {
    if (!s || !url) return false;
    if (s->transport != NP_MCP_HTTP) return true;   /* stdio 不受域名门禁约束 */
    char host[256];
    if (!url_host(url, host, sizeof host)) return false;
    if (list_has(s->domain_block, host)) return false;  /* block 优先 */
    if (!s->domain_allow[0]) return false;              /* fail-closed */
    return list_has(s->domain_allow, host);
}

/* ---------------- 不可信输出清洗 ---------------- */
void np_mcp_sanitize(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!in) { out[0] = 0; return; }
    static const char CUT[] = "...[cut]";
    size_t reserve = sizeof CUT - 1;
    size_t limit = (cap > reserve + 2) ? cap - 1 - reserve : cap - 1;
    size_t n = strlen(in), i = 0, o = 0;
    bool truncated = false;
    while (i < n) {
        if (o >= limit) { truncated = true; break; }
        /* 邮箱：从 local 起始处正向识别整段，避免已写出 local 残留。 */
        bool local_char = isalnum((unsigned char)in[i]) || in[i]=='.' ||
                          in[i]=='_' || in[i]=='+' || in[i]=='-';
        bool prev_local = i > 0 && (isalnum((unsigned char)in[i-1]) || in[i-1]=='.' ||
                          in[i-1]=='_' || in[i-1]=='+' || in[i-1]=='-');
        if (local_char && !prev_local) {
            size_t at = i;
            while (at < n && (isalnum((unsigned char)in[at]) || in[at]=='.' ||
                   in[at]=='_' || in[at]=='+' || in[at]=='-')) ++at;
            if (at < n && in[at] == '@') {
                size_t e = at + 1; bool dot = false;
                while (e < n && (isalnum((unsigned char)in[e]) || in[e]=='.' ||
                       in[e]=='-' || in[e]=='_')) { if (in[e]=='.') dot = true; ++e; }
                if (e > at + 1 && dot && o + 8 < limit) {
                    memcpy(out + o, "[EMAIL]", 7); o += 7; i = e; continue;
                }
            }
        }
        if (isdigit((unsigned char)in[i])) {
            size_t e = i;
            while (e < n && isdigit((unsigned char)in[e])) ++e;
            size_t L = e - i;
            if (((L==17 && e<n && (in[e]=='X'||in[e]=='x')) || L==18) && o+5<limit) {
                memcpy(out + o, "[ID]", 4); o += 4;
                i = (L==17) ? e+1 : e; continue;
            }
            if (L==11 && in[i]=='1' && in[i+1]>='3' && in[i+1]<='9' && o+8<limit) {
                memcpy(out + o, "[PHONE]", 7); o += 7; i = e; continue;
            }
            if (L>=12 && o+6<limit) {
                memcpy(out + o, "[NUM]", 5); o += 5; i = e; continue;
            }
        }
        unsigned char c = (unsigned char)in[i];
        /* 剥离控制字符（保留 \n \r \t）；不可信文本里其余控制字符一律丢弃。 */
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { ++i; continue; }
        out[o++] = in[i++];
    }
    if (truncated && cap > reserve + 2) {
        memcpy(out + o, CUT, reserve); o += reserve;
    }
    out[o] = 0;
}

/* ---------------- 最小 JSON 解析器（从 X9 移植，独立编译） ---------------- */
typedef struct {
    const char *s;
    size_t len, pos, tokens, depth;
} jp_t;
static void jws(jp_t *j) {
    while (j->pos < j->len && isspace((unsigned char)j->s[j->pos])) ++j->pos;
}
/* value kind: 0=str 1=obj 2=arr 3=scalar -1=err */
static int jvalue(jp_t *j, size_t *st, size_t *en) {
    jws(j);
    if (j->pos >= j->len || ++j->tokens > NP_MCP_JSON_MAX_TOKENS) return -1;
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
        if (++j->depth > NP_MCP_JSON_MAX_DEPTH) return -1;
        char close = c == '{' ? '}' : ']';
        ++j->pos;
        while (j->pos < j->len) {
            jws(j);
            if (j->pos < j->len && j->s[j->pos] == close) {
                ++j->pos; --j->depth; *en = j->pos;
                return c == '{' ? 1 : 2;
            }
            if (j->pos < j->len && (j->s[j->pos]=='"' || j->s[j->pos]=='{' || j->s[j->pos]=='[')) {
                size_t a, b;
                if (jvalue(j, &a, &b) < 0) return -1;
            } else ++j->pos;
        }
        return -1;
    }
    while (j->pos < j->len && j->s[j->pos]!=',' && j->s[j->pos]!='}' &&
           j->s[j->pos]!=']' && !isspace((unsigned char)j->s[j->pos])) ++j->pos;
    *en = j->pos;
    return 3;
}
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
/* 按点分路径定位节点（对象成员 / 数组下标） */
static bool jslice(const char *json, size_t json_len, const char *path,
                   size_t *out_st, size_t *out_len, int *out_kind) {
    if (!json || !path) return false;
    jp_t j = { json, json_len, 0, 0, 0 };
    size_t cs, ce; int kind = jvalue(&j, &cs, &ce);
    if (kind < 0) return false;
    if (!path[0]) { *out_st=cs; *out_len=ce-cs; if(out_kind)*out_kind=kind; return true; }
    char seg[NP_MCP_PATH_CAP];
    mcopy(seg, sizeof seg, path);
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
            if (endp == ctx || idx < 0 || *endp) return false;
            long cur = 0; size_t p = cs + 1; bool found = false;
            while (p < ce) {
                p = (size_t)(skip_space(json + p, json + ce) - json);
                if (p >= ce || json[p] == ']') break;
                size_t vs, ve; jp_t sub = { json, ce, p, j.tokens, j.depth };
                int sk = jvalue(&sub, &vs, &ve);
                if (sk < 0) return false;
                j.tokens = sub.tokens; j.depth = sub.depth;
                if (cur == idx) { cs=vs; ce=ve; kind=sk; found=true; break; }
                ++cur; p = (size_t)(skip_space(json + ve, json + ce) - json);
                if (p < ce && json[p] == ',') ++p;
            }
            if (!found) return false;
        } else return false;
        ctx = dot ? dot + 1 : NULL;
    }
    *out_st = cs; *out_len = ce - cs;
    if (out_kind) *out_kind = kind;
    return true;
}
static bool node_string(const char *json, size_t st, size_t len, char *out, size_t cap) {
    if (!json || !out || cap == 0) return false;
    out[0] = 0;
    size_t end = st + len;
    if (len == 0 || st >= end) return false;
    if (json[st] != '"' || end <= st + 1) return false;
    size_t o = 0;
    for (size_t i = st + 1; i + 1 < end && o + 1 < cap; ++i) {
        char ch = json[i];
        if (ch == '\\' && i + 2 < end) {
            char d = json[++i];
            if (d=='n') ch='\n'; else if (d=='r') ch='\r'; else if (d=='t') ch='\t';
            else if (d=='u') { if (i+4<end) { ch='?'; i+=4; } else ch='?'; }
            else ch = d;
        } else if (ch == '"') break;
        out[o++] = ch;
    }
    out[o] = 0;
    return true;
}
void np_mcp_json_escape(const char *s, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!s) { out[0] = 0; return; }
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 6 < cap; ++p) {
        unsigned char c = *p;
        if (c == '"') { memcpy(out+o, "\\\"", 2); o += 2; }
        else if (c == '\\') { memcpy(out+o, "\\\\", 2); o += 2; }
        else if (c == '\n') { memcpy(out+o, "\\n", 2); o += 2; }
        else if (c == '\r') { memcpy(out+o, "\\r", 2); o += 2; }
        else if (c == '\t') { memcpy(out+o, "\\t", 2); o += 2; }
        else if (c < 0x20) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}
bool np_mcp_json_string(const char *json, size_t json_len, const char *path,
                        char *out, size_t cap) {
    size_t st, len; int kind;
    if (!jslice(json, json_len, path, &st, &len, &kind)) { if(out&&cap)out[0]=0; return false; }
    if (kind != 0) { if(out&&cap)out[0]=0; return false; }
    return node_string(json, st, len, out, cap);
}
/* 前置声明：数组元素边界扫描（括号深度 + 字符串/转义状态，不依赖递归 jvalue）。 */
static bool scan_array_elem(const char *json, size_t end, size_t pos,
                            size_t *out_st, size_t *out_en);
int np_mcp_json_array_len(const char *json, size_t json_len, const char *path) {
    size_t st, len; int kind;
    if (!jslice(json, json_len, path, &st, &len, &kind) || kind != 2) return -1;
    size_t end = st + len;      /* 数组节点结束（']' 之后） */
    size_t p = st + 1; int cnt = 0; size_t vs, ve;
    while (scan_array_elem(json, end, p, &vs, &ve)) {
        ++cnt;
        p = (size_t)(skip_space(json + ve, json + end) - json);
        if (p < end && json[p] == ',') ++p;
    }
    return cnt;
}
/* 独立扫描数组元素边界：括号深度 + 字符串/转义状态，不依赖递归 jvalue。 */
static bool scan_array_elem(const char *json, size_t end, size_t pos,
                            size_t *out_st, size_t *out_en) {
    pos = (size_t)(skip_space(json + pos, json + end) - json);
    if (pos >= end || json[pos] == ']') return false;
    size_t st = pos;
    char open = json[pos];
    if (open == '{' || open == '[') {
        char close = open == '{' ? '}' : ']';
        int depth = 0; bool in_str = false;
        for (; pos < end; ++pos) {
            char c = json[pos];
            if (in_str) {
                if (c == '\\') { if (pos + 1 < end) ++pos; }
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == open) ++depth;
            else if (c == close && --depth == 0) { ++pos; break; }
        }
    } else if (open == '"') {
        ++pos;
        for (; pos < end; ++pos) {
            char c = json[pos];
            if (c == '\\') { if (pos + 1 < end) ++pos; }
            else if (c == '"') { ++pos; break; }
        }
    } else {
        while (pos < end && json[pos] != ',' && json[pos] != ']') ++pos;
    }
    *out_st = st; *out_en = pos;
    return true;
}
bool np_mcp_json_array_elem(const char *json, size_t json_len, const char *path,
                            int idx, size_t *out_start, size_t *out_len) {
    size_t st, len; int kind;
    if (!jslice(json, json_len, path, &st, &len, &kind) || kind != 2 || idx < 0) return false;
    size_t end = st + len;
    size_t p = st + 1; int cur = 0; size_t vs, ve;
    while (scan_array_elem(json, end, p, &vs, &ve)) {
        if (cur == idx) { *out_start = vs; *out_len = ve - vs; return true; }
        ++cur;
        p = (size_t)(skip_space(json + ve, json + end) - json);
        if (p < end && json[p] == ',') ++p;
    }
    return false;
}
bool np_mcp_json_member_string(const char *json, size_t start, size_t len,
                               const char *key, char *out, size_t cap) {
    if (!json || !out || cap == 0) return false;
    out[0] = 0;
    size_t end = start + len;
    if (len == 0 || start >= end || json[start] != '{') return false;
    jp_t j = { json, end, 0, 0, 0 };
    size_t vs, ve; int kind;
    if (!jmember(&j, start, end, key, &vs, &ve, &kind) || kind != 0) return false;
    return node_string(json, vs, ve - vs, out, cap);
}

/* ---------------- Store ---------------- */
np_mcp_store_t *np_mcp_store_create(void) {
    np_mcp_store_t *st = (np_mcp_store_t *)calloc(1, sizeof *st);
    return st;
}
void np_mcp_store_destroy(np_mcp_store_t *st) { free(st); }

size_t np_mcp_count(const np_mcp_store_t *st) { return st ? st->n : 0; }
int np_mcp_index_of(const np_mcp_store_t *st, const char *name) {
    if (!st || !name) return -1;
    for (size_t i = 0; i < st->n; ++i)
        if (strcmp(st->items[i].srv.name, name) == 0) return (int)i;
    return -1;
}
const np_mcp_server_t *np_mcp_at(const np_mcp_store_t *st, size_t i) {
    return (st && i < st->n) ? &st->items[i].srv : NULL;
}
const np_mcp_server_t *np_mcp_get(const np_mcp_store_t *st, const char *name) {
    int i = np_mcp_index_of(st, name);
    return i < 0 ? NULL : &st->items[(size_t)i].srv;
}
static np_mcp_item_t *item_of(np_mcp_store_t *st, const char *name) {
    int i = np_mcp_index_of(st, name);
    return i < 0 ? NULL : &st->items[(size_t)i];
}

np_mcp_status_t np_mcp_add(np_mcp_store_t *st, const np_mcp_server_t *s) {
    if (!st || !s) return NP_MCP_ERR_ARG;
    char why[160];
    np_mcp_status_t v = np_mcp_validate(s, why, sizeof why);
    if (v != NP_MCP_OK) return v;
    if (np_mcp_index_of(st, s->name) >= 0) return NP_MCP_ERR_LIMIT;  /* 同名拒绝 */
    if (st->n >= NP_MCP_MAX_SERVERS) return NP_MCP_ERR_LIMIT;
    np_mcp_item_t *it = &st->items[st->n++];
    memset(it, 0, sizeof *it);
    it->srv = *s;
    it->srv.health = false;
    it->srv.fail_streak = 0;
    return NP_MCP_OK;
}
bool np_mcp_remove(np_mcp_store_t *st, const char *name) {
    int idx = np_mcp_index_of(st, name);
    if (idx < 0) return false;
    for (size_t i = (size_t)idx + 1; i < st->n; ++i) st->items[i-1] = st->items[i];
    memset(&st->items[--st->n], 0, sizeof st->items[0]);
    return true;
}
bool np_mcp_set_enabled(np_mcp_store_t *st, const char *name, bool enabled) {
    np_mcp_item_t *it = item_of(st, name);
    if (!it) return false;
    it->srv.enabled = enabled;
    if (enabled) { it->srv.fail_streak = 0; /* 不自动信任健康，需下次检查 */ }
    return true;
}
bool np_mcp_set_allow_write(np_mcp_store_t *st, const char *name, bool allow) {
    np_mcp_item_t *it = item_of(st, name);
    if (!it) return false;
    it->srv.allow_write = allow;
    return true;
}

/* ---------------- 持久化（mcp_servers.ini） ---------------- */
static const char *kv(int transport) { return transport == NP_MCP_STDIO ? "stdio" : "http"; }
bool np_mcp_store_save(const np_mcp_store_t *st, const char *path) {
    if (!st || !path) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    for (size_t i = 0; i < st->n; ++i) {
        const np_mcp_server_t *s = &st->items[i].srv;
        fprintf(f, "[mcp.%u]\n", (unsigned)i);
        fprintf(f, "name=%s\n", s->name);
        fprintf(f, "transport=%s\n", kv(s->transport));
        fprintf(f, "command=%s\n", s->command);
        fprintf(f, "endpoint=%s\n", s->endpoint);
        fprintf(f, "auth_ref=%s\n", s->auth_ref);
        fprintf(f, "domain_allow=%s\n", s->domain_allow);
        fprintf(f, "domain_block=%s\n", s->domain_block);
        fprintf(f, "timeout_sec=%d\n", s->timeout_sec);
        fprintf(f, "enabled=%s\n", s->enabled ? "true" : "false");
        fprintf(f, "health=%s\n", s->health ? "true" : "false");
        fprintf(f, "allow_write=%s\n", s->allow_write ? "true" : "false");
        fprintf(f, "sha256=%s\n\n", s->sha256);
    }
    return fclose(f) == 0;
}
size_t np_mcp_store_load(np_mcp_store_t *st, const char *path) {
    if (!st || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[600];
    np_mcp_server_t cur; memset(&cur, 0, sizeof cur);
    bool in = false; size_t loaded = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = mtrim(line);
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            if (in && np_mcp_add(st, &cur) == NP_MCP_OK) ++loaded;
            memset(&cur, 0, sizeof cur);
            cur.transport = NP_MCP_STDIO;
            cur.timeout_sec = 30;
            in = true; continue;
        }
        if (!in) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = mtrim(p), *v = mtrim(eq + 1);
        if (strcmp(k,"name")==0) mcopy(cur.name,sizeof cur.name,v);
        else if (strcmp(k,"transport")==0) cur.transport = strcmp(v,"http")==0 ? NP_MCP_HTTP : NP_MCP_STDIO;
        else if (strcmp(k,"command")==0) mcopy(cur.command,sizeof cur.command,v);
        else if (strcmp(k,"endpoint")==0) mcopy(cur.endpoint,sizeof cur.endpoint,v);
        else if (strcmp(k,"auth_ref")==0) mcopy(cur.auth_ref,sizeof cur.auth_ref,v);
        else if (strcmp(k,"domain_allow")==0) mcopy(cur.domain_allow,sizeof cur.domain_allow,v);
        else if (strcmp(k,"domain_block")==0) mcopy(cur.domain_block,sizeof cur.domain_block,v);
        else if (strcmp(k,"timeout_sec")==0) { int x=atoi(v); cur.timeout_sec = (x>=1&&x<=120)?x:30; }
        else if (strcmp(k,"enabled")==0) cur.enabled = strcmp(v,"true")==0;
        else if (strcmp(k,"health")==0) cur.health = strcmp(v,"true")==0;
        else if (strcmp(k,"allow_write")==0) cur.allow_write = strcmp(v,"true")==0;
        else if (strcmp(k,"sha256")==0) mcopy(cur.sha256,sizeof cur.sha256,v);
    }
    if (in && np_mcp_add(st, &cur) == NP_MCP_OK) ++loaded;
    fclose(f);
    return loaded;
}

/* ---------------- 运行时：发现 / 健康检查 / 调用 ---------------- */
static bool has_error(const char *json, size_t len) {
    /* 存在 "error" 成员即视为 JSON-RPC 错误（键名带引号，简单字面查找即可）。 */
    if (len > 5 && strstr(json, "\"error\"")) return true;
    (void)len;
    return false;
}
static np_mcp_status_t check_secret(const np_mcp_server_t *s, np_mcp_secret_fn resolve, void *user) {
    if (!s->auth_ref[0]) return NP_MCP_OK;         /* 公开服务无需凭据 */
    if (!resolve) return NP_MCP_OK;                /* 未注入解析器，交给传输实现 */
    if (!resolve(s->auth_ref, user)) return NP_MCP_ERR_TRANSPORT;  /* 凭据不可用 */
    return NP_MCP_OK;
}
static bool looks_write(const char *s) {
    static const char *WORDS[] = { "write","edit","delete","patch","create","update",
                                   "remove","set","save","modify","insert","put","exec","run" };
    char buf[512]; mcopy(buf, sizeof buf, s);
    for (size_t i = 0; i < sizeof WORDS/sizeof WORDS[0]; ++i) {
        size_t w = strlen(WORDS[i]);
        for (char *p = buf; *p; ++p) {
            if (tolower((unsigned char)*p) != WORDS[i][0]) continue;
            if (ci_eq(p, WORDS[i], w)) { size_t n = strlen(p); if (n == w || !isalnum((unsigned char)p[w])) return true; }
        }
    }
    return false;
}

int np_mcp_discover(np_mcp_store_t *st, const char *name,
                    np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                    void *user, np_mcp_status_t *out_status) {
    if (out_status) *out_status = NP_MCP_ERR_ARG;
    np_mcp_item_t *it = item_of(st, name);
    if (!it || !transport) { if(out_status)*out_status=NP_MCP_ERR_ARG; return 0; }
    if (!it->srv.enabled) { if(out_status)*out_status=NP_MCP_ERR_DISABLED; return 0; }
    np_mcp_status_t cs = check_secret(&it->srv, resolve, user);
    if (cs != NP_MCP_OK) { if(out_status)*out_status=cs; return 0; }
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
    char *resp = (char *)malloc(NP_MCP_RESP_CAP);
    if (!resp) { if(out_status)*out_status=NP_MCP_ERR_LIMIT; return 0; }
    resp[0] = 0;
    bool ok = transport(&it->srv, req, resp, NP_MCP_RESP_CAP, user);
    if (!ok) { free(resp); if(out_status)*out_status=NP_MCP_ERR_TRANSPORT; return 0; }
    size_t rlen = strlen(resp);
    if (has_error(resp, rlen)) { free(resp); if(out_status)*out_status=NP_MCP_ERR_TRANSPORT; return 0; }
    int cnt = np_mcp_json_array_len(resp, rlen, "result.tools");
    if (cnt < 0) { free(resp); if(out_status)*out_status=NP_MCP_ERR_PARSE; return 0; }
    if (cnt > NP_MCP_MAX_TOOLS) cnt = NP_MCP_MAX_TOOLS;
    it->tool_count = 0;
    char nm[96], desc[256];
    for (int i = 0; i < cnt; ++i) {
        size_t st_off, e_len;   /* e_len 是元素长度（非结束位置） */
        if (!np_mcp_json_array_elem(resp, rlen, "result.tools", i, &st_off, &e_len)) continue;
        if (!np_mcp_json_member_string(resp, st_off, e_len, "name", nm, sizeof nm)) continue;
        np_mcp_tool_t *t = &it->tools[it->tool_count++];
        memset(t, 0, sizeof *t);
        np_mcp_sanitize(nm, t->name, sizeof t->name);              /* 不可信：脱敏 */
        if (np_mcp_json_member_string(resp, st_off, e_len, "description", desc, sizeof desc))
            np_mcp_sanitize(desc, t->description, sizeof t->description);
        t->write_tool = looks_write(nm) || looks_write(desc);
        t->enabled = true;
    }
    free(resp);
    if (out_status) *out_status = NP_MCP_OK;
    return (int)it->tool_count;
}

bool np_mcp_health_check(np_mcp_store_t *st, const char *name,
                         np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                         void *user) {
    np_mcp_item_t *it = item_of(st, name);
    if (!it || !transport || !it->srv.enabled) return false;
    np_mcp_status_t cs = check_secret(&it->srv, resolve, user);
    if (cs != NP_MCP_OK) {
        if (++it->srv.fail_streak >= NP_MCP_FAIL_LIMIT) it->srv.enabled = false;
        it->srv.health = false; return false;
    }
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"params\":{}}";
    char resp[8192]; resp[0] = 0;   /* ping 响应很小，栈上足够 */
    bool ok = transport(&it->srv, req, resp, sizeof resp, user);
    if (ok && !has_error(resp, strlen(resp)) && strstr(resp, "\"result\"")) {
        it->srv.health = true; it->srv.fail_streak = 0; return true;
    }
    it->srv.health = false;
    if (++it->srv.fail_streak >= NP_MCP_FAIL_LIMIT) it->srv.enabled = false;  /* 失败禁用 */
    return false;
}

np_mcp_status_t np_mcp_call(np_mcp_store_t *st, const char *name,
                            const char *tool, const char *arguments_json,
                            np_mcp_transport_fn transport, np_mcp_secret_fn resolve,
                            void *user, char *out, size_t out_cap) {
    if (out && out_cap) out[0] = 0;
    np_mcp_item_t *it = item_of(st, name);
    if (!it || !tool || !transport) return NP_MCP_ERR_ARG;
    if (!it->srv.enabled || !it->srv.health) return NP_MCP_ERR_DISABLED;
    if (it->srv.transport == NP_MCP_HTTP) {
        /* 用 endpoint 做域名门禁（工具不携带独立 URL）。 */
        if (!np_mcp_domain_allowed(&it->srv, it->srv.endpoint)) return NP_MCP_ERR_DOMAIN_DENIED;
    }
    const np_mcp_tool_t *t = NULL;
    for (size_t i = 0; i < it->tool_count; ++i)
        if (strcmp(it->tools[i].name, tool) == 0) { t = &it->tools[i]; break; }
    if (!t || !t->enabled) return NP_MCP_ERR_NOT_FOUND;
    if (t->write_tool && !it->srv.allow_write) return NP_MCP_ERR_WRITE_DENIED;
    np_mcp_status_t cs = check_secret(&it->srv, resolve, user);
    if (cs != NP_MCP_OK) return cs;

    char esc_name[160];
    np_mcp_json_escape(tool, esc_name, sizeof esc_name);
    char *req = (char *)malloc(NP_MCP_REQ_CAP);
    if (!req) return NP_MCP_ERR_LIMIT;
    size_t at = 0;
    at = bcat(req, NP_MCP_REQ_CAP, at, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"");
    at = bcat(req, NP_MCP_REQ_CAP, at, esc_name);
    at = bcat(req, NP_MCP_REQ_CAP, at, "\",\"arguments\":");
    /* arguments_json 是已序列化的 JSON 对象串，直接内联；空则用 {}。 */
    at = bcat(req, NP_MCP_REQ_CAP, at, arguments_json && *arguments_json ? arguments_json : "{}");
    at = bcat(req, NP_MCP_REQ_CAP, at, "}}");
    (void)at;

    char *resp = (char *)malloc(NP_MCP_RESP_CAP);
    if (!resp) { free(req); return NP_MCP_ERR_LIMIT; }
    resp[0] = 0;
    bool ok = transport(&it->srv, req, resp, NP_MCP_RESP_CAP, user);
    free(req);
    if (!ok) { free(resp); return NP_MCP_ERR_TRANSPORT; }
    size_t rlen = strlen(resp);
    if (has_error(resp, rlen)) { free(resp); return NP_MCP_ERR_TRANSPORT; }
    /* 标准 MCP 文本结果在 result.content.0.text；缺失时退回 whole result 字符串。 */
    char raw[NP_MCP_OUT_CAP];
    if (!np_mcp_json_string(resp, rlen, "result.content.0.text", raw, sizeof raw)) {
        if (!np_mcp_json_string(resp, rlen, "result", raw, sizeof raw)) raw[0] = 0;
    }
    free(resp);
    if (out && out_cap) np_mcp_sanitize(raw, out, out_cap);   /* 不可信输出清洗 */
    return NP_MCP_OK;
}

/* ---------------- 工具视图 ---------------- */
size_t np_mcp_tool_count(const np_mcp_store_t *st, const char *name) {
    np_mcp_item_t *it = item_of((np_mcp_store_t *)st, name);
    return it ? it->tool_count : 0;
}
const np_mcp_tool_t *np_mcp_tool_at(const np_mcp_store_t *st, const char *name, size_t i) {
    np_mcp_item_t *it = item_of((np_mcp_store_t *)st, name);
    return (it && i < it->tool_count) ? &it->tools[i] : NULL;
}
bool np_mcp_set_tool_enabled(np_mcp_store_t *st, const char *name,
                             const char *tool, bool enabled) {
    np_mcp_item_t *it = item_of(st, name);
    if (!it) return false;
    for (size_t i = 0; i < it->tool_count; ++i)
        if (strcmp(it->tools[i].name, tool) == 0) { it->tools[i].enabled = enabled; return true; }
    return false;
}
