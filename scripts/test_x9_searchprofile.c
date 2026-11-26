/* test_x9_searchprofile.c — X9 SearchProfile 红线单测。
 * 编译: gcc -std=c99 -Isrc scripts/test_x9_searchprofile.c src/np_searchprofile.c
 * 纯本地，无网络。 */
#include "np_searchprofile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) { g_pass++; } else { \
    fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void scopy_local(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; }

static np_sp_profile_t base(const char *name) {
    np_sp_profile_t p; memset(&p, 0, sizeof p);
    scopy_local(p.name, name); p.backend = NP_SP_HTTP_JSON; p.enabled = true;
    scopy_local(p.endpoint, "https://api.example.com/search");
    scopy_local(p.method, "GET"); scopy_local(p.auth_ref, "keychain:rapideditor/search/example");
    scopy_local(p.query_param, "q"); scopy_local(p.result_path, "items");
    scopy_local(p.field_title, "title"); scopy_local(p.field_url, "link");
    scopy_local(p.field_snippet, "summary");
    p.max_results = 10; p.timeout_sec = 15;
    scopy_local(p.domain_allow, "api.example.com,*.wiki.example.org");
    scopy_local(p.domain_block, "evil.example.com");
    return p;
}

/* ---- mock 传输 ---- */
typedef struct {
    const char *reply;
    int calls;
    char last_url[600];
    char last_secret[128];
} mock_t;
static char *mock_fetch(const char *url, const char *method, const char *secret,
                        int timeout, void *user, int *status) {
    (void)method; (void)timeout;
    mock_t *m = (mock_t *)user;
    m->calls++;
    scopy_local(m->last_url, url);
    scopy_local(m->last_secret, secret ? secret : "");
    if (status) *status = 200;
    size_t rl = strlen(m->reply) + 1;
    char *b = (char *)malloc(rl);
    if (b) memcpy(b, m->reply, rl);
    return b;
}
static const char *mock_resolve(const char *ref, void *user) {
    (void)ref; (void)user; return "RESOLVED-TOKEN-FROM-KEYCHAIN";
}
static size_t mock_local(const char *q, char out[][260], size_t cap, void *user) {
    (void)q; (void)user;
    if (cap < 2) return cap;
    scopy_local(out[0], "src/main.c"); scopy_local(out[1], "docs/guide.md");
    return 2;
}

static void t_validate(void) {
    char why[128];
    np_sp_profile_t p = base("valid");
    CHECK(np_sp_validate(&p, why, sizeof why) == NP_SP_OK);
    /* endpoint 非 http */
    np_sp_profile_t bad = p; scopy_local(bad.endpoint, "ftp://api.example.com");
    CHECK(np_sp_validate(&bad, why, sizeof why) == NP_SP_ERR_BAD_URL);
    /* max_results / timeout 越界 */
    bad = p; bad.max_results = 0;
    CHECK(np_sp_validate(&bad, why, sizeof why) == NP_SP_ERR_LIMIT);
    bad = p; bad.timeout_sec = 999;
    CHECK(np_sp_validate(&bad, why, sizeof why) == NP_SP_ERR_LIMIT);
    /* method 非法 */
    bad = p; scopy_local(bad.method, "DELETE");
    CHECK(np_sp_validate(&bad, why, sizeof why) == NP_SP_ERR_ARG);
    /* 缺名字 */
    bad = p; bad.name[0] = 0;
    CHECK(np_sp_validate(&bad, why, sizeof why) == NP_SP_ERR_ARG);
}

static void t_secret_leak(void) {
    const char *reason = NULL;
    CHECK(np_sp_looks_like_secret("sk-abc123XYZ789def456ghi", &reason));
    CHECK(reason != NULL);
    CHECK(np_sp_looks_like_secret("github_pat_11ABCDEFG0123456789012345", &reason));
    CHECK(np_sp_looks_like_secret("Bearer abcdefghijklmnopqrstuvwxyz", &reason));
    CHECK(np_sp_looks_like_secret("api_key=abcdefghijklmnopqrstuvwxyz", &reason));
    CHECK(!np_sp_looks_like_secret("keychain:rapideditor/search", &reason));
    CHECK(!np_sp_looks_like_secret("env:SEARCH_API_KEY", &reason));
    char why[128];
    np_sp_profile_t p = base("leak");
    scopy_local(p.auth_ref, "sk-live-plaintext-secret-should-be-rejected");
    CHECK(np_sp_validate(&p, why, sizeof why) == NP_SP_ERR_SECRET_LEAK);
}

static void t_store_roundtrip(void) {
    np_sp_store_t *st = np_sp_store_create();
    np_sp_profile_t p = base("alpha");
    CHECK(np_sp_add(st, &p) == NP_SP_OK);
    np_sp_profile_t q = base("beta");
    q.backend = NP_SP_LOCAL_FTS; q.endpoint[0] = 0;
    CHECK(np_sp_add(st, &q) == NP_SP_OK);
    CHECK(np_sp_count(st) == 2);
    CHECK(np_sp_set_enabled(st, "alpha", false));
    CHECK(np_sp_get(st, "alpha") && !np_sp_get(st, "alpha")->enabled);
    const char *path = "tests/golden/_x9_profiles.tmp";
    CHECK(np_sp_store_save(st, path));
    /* 明文密钥不落盘：文件里不应出现 secret 字样 */
    FILE *f = fopen(path, "rb"); CHECK(f != NULL);
    char content[4096]; size_t n = f ? fread(content, 1, sizeof content - 1, f) : 0;
    if (f) { content[n] = 0; fclose(f); }
    CHECK(strstr(content, "sk-live") == NULL);
    CHECK(strstr(content, "auth_ref=keychain:rapideditor/search/example") != NULL);
    CHECK(strstr(content, "backend=local_fts") != NULL);
    np_sp_store_t *st2 = np_sp_store_create();
    CHECK(np_sp_store_load(st2, path) == 2);
    const np_sp_profile_t *a2 = np_sp_get(st2, "alpha");
    CHECK(a2 && !a2->enabled);
    CHECK(a2 && strcmp(a2->result_path, "items") == 0);
    const np_sp_profile_t *b2 = np_sp_get(st2, "beta");
    CHECK(b2 && b2->backend == NP_SP_LOCAL_FTS);
    CHECK(np_sp_remove(st2, "alpha"));
    CHECK(np_sp_count(st2) == 1);
    np_sp_store_destroy(st); np_sp_store_destroy(st2);
    remove(path);
}

static void t_domain(void) {
    np_sp_profile_t p = base("d");
    CHECK(np_sp_domain_allowed(&p, "https://api.example.com/search?q=x"));
    CHECK(np_sp_domain_allowed(&p, "https://sub.wiki.example.org/a"));  /* *. 通配 */
    CHECK(!np_sp_domain_allowed(&p, "https://evil.example.com/x"));      /* block 优先 */
    CHECK(!np_sp_domain_allowed(&p, "https://attacker.net/x"));          /* 不在 allow */
    CHECK(!np_sp_domain_allowed(&p, "not-a-url"));
    np_sp_profile_t closed = p; closed.domain_allow[0] = 0;
    CHECK(!np_sp_domain_allowed(&closed, "https://api.example.com"));    /* fail-closed */
}

static void t_redact(void) {
    char out[256];
    np_sp_redact("mail alice@example.com and bob.lee+tag@sub.example.org here", out, sizeof out);
    CHECK(strstr(out, "[EMAIL]") != NULL);
    CHECK(strstr(out, "alice@") == NULL);
    CHECK(strstr(out, "bob.lee") == NULL);
    np_sp_redact("phone 13812345678 id 11010119900307123X card 6222021234567890123", out, sizeof out);
    CHECK(strstr(out, "[PHONE]") != NULL);
    CHECK(strstr(out, "[ID]") != NULL);
    CHECK(strstr(out, "[NUM]") != NULL);
    CHECK(strstr(out, "13812345678") == NULL);
    /* 普通文本不变 */
    np_sp_redact("hello world 42", out, sizeof out);
    CHECK(strcmp(out, "hello world 42") == 0);
}

static void t_json(void) {
    const char *j = "{\"data\":{\"items\":[{\"title\":\"Alpha\",\"link\":\"https://a.example.com/x\",\"summary\":\"hi\"},"
                    "{\"title\":\"Beta\",\"link\":\"https://b.example.com/y\",\"summary\":\"yo\"}]}}";
    size_t s, l;
    CHECK(np_sp_json_slice(j, strlen(j), "data.items", &s, &l));
    CHECK(l > 0 && j[s] == '[');
    CHECK(np_sp_json_slice(j, strlen(j), "data.items.0.title", &s, &l));
    char v[64];
    CHECK(np_sp_json_string(j, s, l, "x", v, sizeof v) && strcmp(v, "Alpha") == 0);
    /* 从对象节点取字段 */
    CHECK(np_sp_json_slice(j, strlen(j), "data.items.1", &s, &l));
    CHECK(np_sp_json_string(j, s, l, "title", v, sizeof v) && strcmp(v, "Beta") == 0);
    CHECK(np_sp_json_string(j, s, l, "link", v, sizeof v) && strcmp(v, "https://b.example.com/y") == 0);
    /* 不存在的路径 */
    CHECK(!np_sp_json_slice(j, strlen(j), "data.missing", &s, &l));
}

static void t_recall_http(void) {
    mock_t m = {0};
    m.reply = "{\"items\":[{\"title\":\"mail me@secret.com\",\"link\":\"https://a.example.com/1\",\"summary\":\"call 13800000000\"},"
              "{\"title\":\"Second\",\"link\":\"https://a.example.com/2\",\"summary\":\"plain\"}]}";
    np_sp_profile_t p = base("web");
    np_sp_hit_t hits[4]; int total = 0;
    np_sp_status_t s = np_sp_recall_http(&p, "hello world", mock_fetch, mock_resolve, &m,
                                         hits, 4, &total);
    CHECK(s == NP_SP_OK);
    CHECK(m.calls == 1);
    CHECK(strstr(m.last_url, "q=hello%20world") != NULL);
    CHECK(strcmp(m.last_secret, "RESOLVED-TOKEN-FROM-KEYCHAIN") == 0);
    CHECK(total == 2);
    CHECK(strstr(hits[0].title, "[EMAIL]") != NULL);       /* 标题脱敏 */
    CHECK(strstr(hits[0].snippet, "[PHONE]") != NULL);     /* 摘要脱敏 */
    CHECK(strcmp(hits[1].title, "Second") == 0);
    CHECK(strcmp(hits[1].source, "web") == 0);
    /* 域名拒绝：不调用 fetch */
    m.calls = 0;
    np_sp_profile_t denied = p; scopy_local(denied.domain_allow, "other.example.com");
    s = np_sp_recall_http(&denied, "x", mock_fetch, mock_resolve, &m, hits, 4, &total);
    CHECK(s == NP_SP_ERR_DOMAIN_DENIED);
    CHECK(m.calls == 0);
    /* 上限：max_results=1 → total 钳到 1 */
    np_sp_profile_t cap = p; cap.max_results = 1;
    CHECK(np_sp_recall_http(&cap, "x", mock_fetch, mock_resolve, &m, hits, 4, &total) == NP_SP_OK);
    CHECK(total == 1);
}

static void t_recall_local(void) {
    np_sp_profile_t p = base("local");
    p.backend = NP_SP_LOCAL_FTS; p.endpoint[0] = 0;
    np_sp_hit_t hits[4]; int total = 0;
    CHECK(np_sp_recall_local(&p, "main", mock_local, NULL, hits, 4, &total) == NP_SP_OK);
    CHECK(total == 2);
    CHECK(strcmp(hits[0].title, "src/main.c") == 0);
    CHECK(strcmp(hits[1].url, "docs/guide.md") == 0);
}

static void t_chain(void) {
    np_sp_store_t *st = np_sp_store_create();
    /* primary: HTTP 但域名不允许（失败）；fallback → local（成功） */
    np_sp_profile_t web = base("web");
    scopy_local(web.domain_allow, "nowhere.example.com");
    scopy_local(web.fallback, "local");
    CHECK(np_sp_add(st, &web) == NP_SP_OK);
    np_sp_profile_t loc = base("local");
    loc.backend = NP_SP_LOCAL_FTS; loc.endpoint[0] = 0; loc.domain_allow[0] = 0;
    CHECK(np_sp_add(st, &loc) == NP_SP_OK);
    mock_t m = {0}; m.reply = "{}";
    np_sp_hit_t hits[4]; int total = 0; char used[64];
    CHECK(np_sp_recall_chain(st, "web", "q", mock_fetch, mock_resolve, mock_local,
                             &m, hits, 4, &total, used, sizeof used) == NP_SP_OK);
    CHECK(strcmp(used, "local") == 0);
    CHECK(total == 2);
    /* 环：web→local→web */
    np_sp_profile_t loop = web; scopy_local(loop.fallback, "web");
    CHECK(np_sp_set_enabled(st, "local", true));
    CHECK(np_sp_add(st, &loop) == NP_SP_OK);
    np_sp_profile_t l2 = loc; scopy_local(l2.fallback, "local");
    CHECK(np_sp_add(st, &l2) == NP_SP_OK);
    /* 两个互相指向：a->b, b->a */
    np_sp_store_t *cyc = np_sp_store_create();
    np_sp_profile_t a = base("a"); scopy_local(a.fallback, "b");
    np_sp_profile_t b = base("b"); scopy_local(b.fallback, "a");
    CHECK(np_sp_add(cyc, &a) == NP_SP_OK);
    CHECK(np_sp_add(cyc, &b) == NP_SP_OK);
    CHECK(np_sp_recall_chain(cyc, "a", "q", mock_fetch, mock_resolve, mock_local,
                             &m, hits, 4, &total, used, sizeof used) == NP_SP_ERR_CYCLE);
    np_sp_store_destroy(st); np_sp_store_destroy(cyc);
}

int main(void) {
    t_validate();
    t_secret_leak();
    t_store_roundtrip();
    t_domain();
    t_redact();
    t_json();
    t_recall_http();
    t_recall_local();
    t_chain();
    printf("X9 SEARCHPROFILE: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
