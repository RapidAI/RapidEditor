/* test_x6_mcp.c — X6 MCP 工具服务器单测。
 * 编译: gcc -std=c99 -Isrc scripts/test_x6_mcp.c src/np_mcp.c
 * 纯本地、零网络：stdio/HTTP 传输与密钥解析全部注入 mock。 */
#include "np_mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) { g_pass++; } else { \
    fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void cpy(char *d, size_t cap, const char *s) {
    if (!d || cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

static np_mcp_server_t base_http(const char *name) {
    np_mcp_server_t s; memset(&s, 0, sizeof s);
    cpy(s.name, sizeof s.name, name);
    s.transport = NP_MCP_HTTP; s.enabled = true; s.timeout_sec = 15;
    cpy(s.endpoint, sizeof s.endpoint, "https://kb.example.com/mcp");
    cpy(s.auth_ref, sizeof s.auth_ref, "keychain:rapideditor/mcp/kb");
    cpy(s.domain_allow, sizeof s.domain_allow, "kb.example.com,*.internal.example.org");
    cpy(s.domain_block, sizeof s.domain_block, "evil.example.com");
    return s;
}

/* ---- mock 传输：按 method 返回不同 JSON-RPC 响应 ---- */
typedef struct {
    const char *tools_reply;   /* tools/list 响应 */
    const char *call_reply;    /* tools/call 响应 */
    bool        ping_ok;
    int         calls;
    int         fail_until;    /* 前 N 次调用失败（模拟掉线） */
    char        last_request[2048];
} mock_t;

static bool mock_transport(const np_mcp_server_t *srv, const char *req,
                           char *out, size_t cap, void *user) {
    (void)srv;
    mock_t *m = (mock_t *)user;
    m->calls++;
    cpy(m->last_request, sizeof m->last_request, req);
    if (m->calls <= m->fail_until) { cpy(out, cap, ""); return false; }
    if (strstr(req, "tools/list")) {
        cpy(out, cap, m->tools_reply ? m->tools_reply : "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[]}}");
    } else if (strstr(req, "tools/call")) {
        cpy(out, cap, m->call_reply ? m->call_reply : "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}");
    } else if (strstr(req, "ping")) {
        cpy(out, cap, m->ping_ok ? "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}"
                                 : "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-1}}");
    } else {
        cpy(out, cap, "{}");
    }
    return true;
}
static const char *mock_resolve(const char *ref, void *user) {
    (void)ref; (void)user; return "RESOLVED-TOKEN";
}

/* ---- 校验红线 ---- */
static void t_validate(void) {
    char why[160];
    np_mcp_server_t s = base_http("kb");

    CHECK(np_mcp_validate(&s, why, sizeof why) == NP_MCP_OK);

    /* 明文密钥形态 → SECRET_LEAK */
    np_mcp_server_t bad = s;
    cpy(bad.auth_ref, sizeof bad.auth_ref, "sk-abcdefghijklmnop1234567890");
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_SECRET_LEAK);
    /* Bearer 内联 */
    bad = s;
    cpy(bad.endpoint, sizeof bad.endpoint, "https://kb.example.com/?k=Bearer%20abcdefghijklmnop");
    /* 上面含 URL，Bearer 后是被编码串，形态检测按空格匹配未必命中；改测 auth_ref inline */
    bad = s;
    cpy(bad.auth_ref, sizeof bad.auth_ref, "token abcdefghijklmnopqrst");
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_SECRET_LEAK);

    /* auth_ref 非引用形态 */
    bad = s; cpy(bad.auth_ref, sizeof bad.auth_ref, "my-secret-inline");
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_BAD_CONFIG);

    /* endpoint 非 http(s) */
    bad = s; cpy(bad.endpoint, sizeof bad.endpoint, "ftp://kb.example.com/mcp");
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_BAD_CONFIG);

    /* timeout 越界 */
    bad = s; bad.timeout_sec = 0;
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_LIMIT);
    bad = s; bad.timeout_sec = 121;
    CHECK(np_mcp_validate(&bad, why, sizeof why) == NP_MCP_ERR_LIMIT);

    /* stdio: 命令空 / shell 元字符 */
    np_mcp_server_t std = base_http("local"); std.transport = NP_MCP_STDIO;
    cpy(std.command, sizeof std.command, "");
    CHECK(np_mcp_validate(&std, why, sizeof why) == NP_MCP_ERR_BAD_CONFIG);
    std = base_http("local"); std.transport = NP_MCP_STDIO;
    cpy(std.command, sizeof std.command, "python server.py; rm -rf /");
    CHECK(np_mcp_validate(&std, why, sizeof why) == NP_MCP_ERR_BAD_CONFIG);
    std = base_http("local"); std.transport = NP_MCP_STDIO;
    cpy(std.command, sizeof std.command, "python server.py --stdio");
    CHECK(np_mcp_validate(&std, why, sizeof why) == NP_MCP_OK);
}

static void t_secret_shapes(void) {
    const char *r = NULL;
    CHECK(np_mcp_looks_like_secret("sk-abcdefghijklmnopqrst", &r));
    CHECK(np_mcp_looks_like_secret("ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", &r));
    CHECK(np_mcp_looks_like_secret("Bearer abcdefghijklmnopqrst", &r));
    /* 引用形态不命中 */
    CHECK(!np_mcp_looks_like_secret("keychain:rapideditor/mcp/kb", &r));
    CHECK(!np_mcp_looks_like_secret("env:MCP_TOKEN", &r));
    CHECK(!np_mcp_looks_like_secret("vault://secret/mcp", &r));
    CHECK(!np_mcp_looks_like_secret("", &r));
}

static void t_domain(void) {
    np_mcp_server_t s = base_http("kb");
    CHECK(np_mcp_domain_allowed(&s, "https://kb.example.com/mcp"));
    CHECK(np_mcp_domain_allowed(&s, "https://wiki.internal.example.org/x"));
    CHECK(!np_mcp_domain_allowed(&s, "https://evil.example.com/mcp"));     /* block 优先 */
    CHECK(!np_mcp_domain_allowed(&s, "https://unknown.other.com/x"));      /* fail-closed */
    CHECK(!np_mcp_domain_allowed(&s, "ftp://kb.example.com/x"));           /* 非 http */
    /* allow 空 = fail-closed */
    np_mcp_server_t b = s; cpy(b.domain_allow, sizeof b.domain_allow, "");
    CHECK(!np_mcp_domain_allowed(&b, "https://kb.example.com/mcp"));
    /* stdio 不受域名门禁约束 */
    b = s; b.transport = NP_MCP_STDIO;
    CHECK(np_mcp_domain_allowed(&b, "anything"));
}

/* ---- store 增删启停 + 持久化 ---- */
static void t_store(void) {
    np_mcp_store_t *st = np_mcp_store_create();
    CHECK(st != NULL);
    CHECK(np_mcp_count(st) == 0);

    np_mcp_server_t a = base_http("alpha");
    np_mcp_server_t b = base_http("beta");
    CHECK(np_mcp_add(st, &a) == NP_MCP_OK);
    CHECK(np_mcp_add(st, &b) == NP_MCP_OK);
    CHECK(np_mcp_count(st) == 2);
    CHECK(np_mcp_add(st, &a) == NP_MCP_ERR_LIMIT);         /* 同名拒绝 */
    CHECK(np_mcp_index_of(st, "beta") == 1);
    CHECK(np_mcp_get(st, "alpha") != NULL);
    CHECK(np_mcp_get(st, "nope") == NULL);

    CHECK(np_mcp_set_enabled(st, "alpha", false));
    CHECK(!np_mcp_get(st, "alpha")->enabled);
    CHECK(np_mcp_set_allow_write(st, "alpha", true));
    CHECK(np_mcp_get(st, "alpha")->allow_write);

    /* 持久化往返（只存引用，不存明文） */
    const char *path = "t_x6_mcp_servers.ini";
    CHECK(np_mcp_store_save(st, path));
    np_mcp_store_t *st2 = np_mcp_store_create();
    CHECK(np_mcp_store_load(st2, path) == 2);
    CHECK(np_mcp_count(st2) == 2);
    const np_mcp_server_t *g = np_mcp_get(st2, "alpha");
    CHECK(g && !g->enabled && g->allow_write);
    CHECK(g && strcmp(g->auth_ref, "keychain:rapideditor/mcp/kb") == 0);
    np_mcp_store_destroy(st2);
    remove(path);

    CHECK(np_mcp_remove(st, "alpha"));
    CHECK(np_mcp_count(st) == 1);
    CHECK(!np_mcp_remove(st, "nope"));
    np_mcp_store_destroy(st);
}

/* ---- 动态发现 ---- */
static void t_discover(void) {
    np_mcp_store_t *st = np_mcp_store_create();
    np_mcp_server_t s = base_http("tools");
    np_mcp_add(st, &s);

    mock_t m; memset(&m, 0, sizeof m);
    m.ping_ok = true;
    m.tools_reply =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":["
        "{\"name\":\"search\",\"description\":\"read only lookup\"},"
        "{\"name\":\"write_file\",\"description\":\"modify a file on disk\"},"
        "{\"name\":\"ping_me\",\"description\":\"say hi\"}"
        "]}}";
    np_mcp_status_t stt = NP_MCP_ERR_ARG;
    int n = np_mcp_discover(st, "tools", mock_transport, mock_resolve, &m, &stt);
    CHECK(stt == NP_MCP_OK);
    CHECK(n == 3);
    CHECK(np_mcp_tool_count(st, "tools") == 3);
    const np_mcp_tool_t *t0 = np_mcp_tool_at(st, "tools", 0);
    const np_mcp_tool_t *t1 = np_mcp_tool_at(st, "tools", 1);
    CHECK(t0 && strcmp(t0->name, "search") == 0 && !t0->write_tool);
    CHECK(t1 && strcmp(t1->name, "write_file") == 0 && t1->write_tool);  /* 写工具识别 */
    CHECK(strstr(m.last_request, "tools/list") != NULL);
    CHECK(strstr(m.last_request, "keychain") == NULL);                    /* 密钥不进请求 */

    /* 未启用 → DISABLED */
    np_mcp_set_enabled(st, "tools", false);
    n = np_mcp_discover(st, "tools", mock_transport, mock_resolve, &m, &stt);
    CHECK(n == 0 && stt == NP_MCP_ERR_DISABLED);

    np_mcp_store_destroy(st);
}

/* ---- 不可信输出脱敏 + 发现阶段清洗 ---- */
static void t_sanitize(void) {
    char out[256];
    np_mcp_sanitize("mail bob@example.com phone 13812345678 num 123456789012 id 110101199003078888",
                    out, sizeof out);
    CHECK(strstr(out, "[EMAIL]") != NULL);
    CHECK(strstr(out, "[PHONE]") != NULL);
    CHECK(strstr(out, "[NUM]") != NULL);
    CHECK(strstr(out, "[ID]") != NULL);
    CHECK(strstr(out, "bob@") == NULL);                 /* 邮箱 local 不残留 */
    CHECK(strstr(out, "13812345678") == NULL);

    /* 控制字符剥离 */
    np_mcp_sanitize("a\x01\x02""b\nc", out, sizeof out);
    CHECK(strstr(out, "ab") != NULL);
    CHECK(strstr(out, "\x01") == NULL);

    /* 有界截断带标注 */
    char big[6000];
    for (int i = 0; i < 5999; ++i) big[i] = 'x';
    big[5999] = 0;
    char small[64];
    np_mcp_sanitize(big, small, sizeof small);
    CHECK(strlen(small) <= 63);
    CHECK(strstr(small, "[cut]") != NULL);

    /* 发现阶段：工具描述里的 PII 被清洗 */
    np_mcp_store_t *st = np_mcp_store_create();
    np_mcp_server_t s = base_http("d");
    np_mcp_add(st, &s);
    mock_t m; memset(&m, 0, sizeof m); m.ping_ok = true;
    m.tools_reply = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":["
                    "{\"name\":\"lookup\",\"description\":\"contact a@b.com now\"}]}}";
    np_mcp_discover(st, "d", mock_transport, mock_resolve, &m, NULL);
    const np_mcp_tool_t *t = np_mcp_tool_at(st, "d", 0);
    CHECK(t && strstr(t->description, "[EMAIL]") != NULL);
    CHECK(t && strstr(t->description, "a@b.com") == NULL);
    np_mcp_store_destroy(st);
}

/* ---- 健康检查：成功 / 失败累计禁用 ---- */
static void t_health(void) {
    np_mcp_store_t *st = np_mcp_store_create();
    np_mcp_server_t s = base_http("h");
    np_mcp_add(st, &s);
    mock_t m; memset(&m, 0, sizeof m); m.ping_ok = true;

    CHECK(np_mcp_health_check(st, "h", mock_transport, mock_resolve, &m));
    CHECK(np_mcp_get(st, "h")->health);
    CHECK(np_mcp_get(st, "h")->fail_streak == 0);

    /* 掉线：连续失败 3 次自动禁用 */
    m.ping_ok = false;
    CHECK(!np_mcp_health_check(st, "h", mock_transport, mock_resolve, &m));
    CHECK(!np_mcp_health_check(st, "h", mock_transport, mock_resolve, &m));
    CHECK(np_mcp_get(st, "h")->enabled);                 /* 第 2 次仍启用 */
    CHECK(!np_mcp_health_check(st, "h", mock_transport, mock_resolve, &m));
    CHECK(!np_mcp_get(st, "h")->enabled);                /* 第 3 次禁用 */
    CHECK(np_mcp_get(st, "h")->fail_streak == NP_MCP_FAIL_LIMIT);

    np_mcp_store_destroy(st);
}

/* ---- 调用：门禁 + 不可信结果 ---- */
static void t_call(void) {
    np_mcp_store_t *st = np_mcp_store_create();
    np_mcp_server_t s = base_http("c");
    np_mcp_add(st, &s);
    mock_t m; memset(&m, 0, sizeof m); m.ping_ok = true;
    m.tools_reply = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":["
                    "{\"name\":\"read\",\"description\":\"read only\"},"
                    "{\"name\":\"write_file\",\"description\":\"write a file\"}]}}";
    np_mcp_discover(st, "c", mock_transport, mock_resolve, &m, NULL);
    CHECK(np_mcp_health_check(st, "c", mock_transport, mock_resolve, &m));

    char out[512];
    /* 未发现工具 */
    CHECK(np_mcp_call(st, "c", "missing", "{}", mock_transport, mock_resolve, &m, out, sizeof out)
          == NP_MCP_ERR_NOT_FOUND);
    /* 写工具默认禁调用 */
    CHECK(np_mcp_call(st, "c", "write_file", "{}", mock_transport, mock_resolve, &m, out, sizeof out)
          == NP_MCP_ERR_WRITE_DENIED);
    /* 显式允许后可调用 */
    np_mcp_set_allow_write(st, "c", true);

    /* 只读工具正常调用，结果脱敏 */
    m.call_reply = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\","
                   "\"text\":\"found contact bob@example.com in docs\"}]}}";
    CHECK(np_mcp_call(st, "c", "read", "{\"q\":\"x\"}", mock_transport, mock_resolve, &m, out, sizeof out)
          == NP_MCP_OK);
    CHECK(strstr(out, "[EMAIL]") != NULL);
    CHECK(strstr(out, "bob@") == NULL);
    CHECK(strstr(m.last_request, "\"method\":\"tools/call\"") != NULL);
    CHECK(strstr(m.last_request, "\"arguments\":{\"q\":\"x\"}") != NULL);

    /* 未健康检查 → DISABLED */
    np_mcp_store_t *st2 = np_mcp_store_create();
    np_mcp_server_t s2 = base_http("c2"); np_mcp_add(st2, &s2);
    CHECK(np_mcp_call(st2, "c2", "read", "{}", mock_transport, mock_resolve, &m, out, sizeof out)
          == NP_MCP_ERR_DISABLED);
    np_mcp_store_destroy(st2);

    /* 域名门禁拒绝（HTTP profile allow 空） */
    np_mcp_store_t *st3 = np_mcp_store_create();
    np_mcp_server_t s3 = base_http("c3"); cpy(s3.domain_allow, sizeof s3.domain_allow, "");
    np_mcp_add(st3, &s3);
    np_mcp_discover(st3, "c3", mock_transport, mock_resolve, &m, NULL);
    np_mcp_health_check(st3, "c3", mock_transport, mock_resolve, &m);
    CHECK(np_mcp_call(st3, "c3", "read", "{}", mock_transport, mock_resolve, &m, out, sizeof out)
          == NP_MCP_ERR_DOMAIN_DENIED);
    np_mcp_store_destroy(st3);

    np_mcp_store_destroy(st);
}

/* ---- JSON-RPC 辅助 ---- */
static void t_json(void) {
    const char *j = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hello\\nworld\"}]}}";
    char out[64];
    CHECK(np_mcp_json_string(j, strlen(j), "result.content.0.text", out, sizeof out));
    CHECK(strcmp(out, "hello\nworld") == 0);
    CHECK(!np_mcp_json_string(j, strlen(j), "result.nope", out, sizeof out));

    char esc[64];
    np_mcp_json_escape("a\"b\\c\nd", esc, sizeof esc);
    CHECK(strcmp(esc, "a\\\"b\\\\c\\nd") == 0);

    const char *arr = "{\"result\":{\"tools\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}]}}";
    CHECK(np_mcp_json_array_len(arr, strlen(arr), "result.tools") == 3);
    size_t st, len;
    CHECK(np_mcp_json_array_elem(arr, strlen(arr), "result.tools", 1, &st, &len));
    CHECK(np_mcp_json_member_string(arr, st, len, "name", out, sizeof out));
    CHECK(strcmp(out, "b") == 0);
}

int main(void) {
    t_validate();
    t_secret_shapes();
    t_domain();
    t_store();
    t_discover();
    t_sanitize();
    t_health();
    t_call();
    t_json();
    printf("X6 MCP: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
